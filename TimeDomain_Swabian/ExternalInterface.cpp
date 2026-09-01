#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <iostream>

#include "TDMeasurement.h"
#include "ExternalInterface.h"
#include "windows.h"

#define member_size(type, member) (sizeof( ((type *)0)->member ))
#define PACKED_SIZE (member_size(MacroMicro_t, macroTime) + member_size(MacroMicro_t, microTime))

typedef struct {
	FILE* file;
	size_t bufferSizeElements;
	size_t bufferElements;
	char* buffer;
	int detectorChannel;
} FileWriteData;

// 1. Thread State Encapsulation (Eliminates Static Globals)
struct MeasurementWrapper {
	TDMeasurement* measurement;
	bool enableFileWrite = false;
	FileWriteData* fileWriteDatas = nullptr;
	size_t fileWriteDatasLength = 0;

	// O(1) lookup table for file writer
	int channelToIndex[256];

	std::queue<std::vector<MacroMicro_t>> diskQueue;
	std::queue<std::vector<MacroMicro_t>> recycleQueue; // Fixes Heap Thrashing
	std::mutex diskMutex;
	std::condition_variable diskCV;
	std::atomic<bool> diskThreadRunning{ false };
	std::thread diskThread;

	std::vector<MacroMicro_t> noFileBuffer; // Persistent buffer for non-saving runs
};

static void fileWriterWorker(MeasurementWrapper* wrapper) {
	while (wrapper->diskThreadRunning || !wrapper->diskQueue.empty()) {
		std::vector<MacroMicro_t> batch;
		{
			std::unique_lock<std::mutex> lock(wrapper->diskMutex);
			wrapper->diskCV.wait(lock, [wrapper] { return !wrapper->diskQueue.empty() || !wrapper->diskThreadRunning; });
			if (!wrapper->diskThreadRunning && wrapper->diskQueue.empty()) break;

			batch = std::move(wrapper->diskQueue.front());
			wrapper->diskQueue.pop();
		}

		for (const auto& d : batch) {
			int channelIndex = wrapper->channelToIndex[static_cast<uint8_t>(d.channel)];
			if (channelIndex < 0) continue;

			if (wrapper->fileWriteDatas[channelIndex].bufferElements == wrapper->fileWriteDatas[channelIndex].bufferSizeElements) {
				char* temp = (char*)realloc(wrapper->fileWriteDatas[channelIndex].buffer, 2 * wrapper->fileWriteDatas[channelIndex].bufferSizeElements * PACKED_SIZE);
				if (temp) {
					wrapper->fileWriteDatas[channelIndex].buffer = temp;
					wrapper->fileWriteDatas[channelIndex].bufferSizeElements *= 2;
				}
			}
			char* buffer = wrapper->fileWriteDatas[channelIndex].buffer + wrapper->fileWriteDatas[channelIndex].bufferElements * PACKED_SIZE;
			memcpy(buffer, &d.macroTime, sizeof(d.macroTime));
			memcpy(buffer + sizeof(d.macroTime), &d.microTime, sizeof(d.microTime));
			wrapper->fileWriteDatas[channelIndex].bufferElements++;
		}
		for (size_t x = 0; x < wrapper->fileWriteDatasLength; x++) {
			if (wrapper->fileWriteDatas[x].bufferElements > 0) {
				fwrite(wrapper->fileWriteDatas[x].buffer, PACKED_SIZE, wrapper->fileWriteDatas[x].bufferElements, wrapper->fileWriteDatas[x].file);
				wrapper->fileWriteDatas[x].bufferElements = 0;
			}
		}

		// Return capacity to the recycle queue
		batch.clear();
		{
			std::lock_guard<std::mutex> lock(wrapper->diskMutex);
			wrapper->recycleQueue.push(std::move(batch));
		}
	}
}

void* getTagger() {
	try {
		return createTimeTagger();
	}
	catch (...) {
		return NULL;
	}
}

void freeTagger(void* tagger) {
	freeTimeTagger(static_cast<TimeTagger*>(tagger));
}

void* newMeasurement(void* tagger, MeasurementParams_t params, const char* directory) {
	MeasurementWrapper* wrapper = new MeasurementWrapper();
	wrapper->enableFileWrite = (directory != NULL);

	std::set<channel_t> detectorChannelSet;
	for (size_t x = 0; x < params.detectorChannelsLength; x++) {
		detectorChannelSet.insert(params.detectorChannels[x]);
	}

	if (wrapper->enableFileWrite) {
		wrapper->fileWriteDatasLength = params.detectorChannelsLength;
		wrapper->fileWriteDatas = (FileWriteData*)malloc(wrapper->fileWriteDatasLength * sizeof(*wrapper->fileWriteDatas));

		// Initialize lookup array to -1
		std::fill_n(wrapper->channelToIndex, 256, -1);

		if (wrapper->fileWriteDatas == NULL) {
			delete wrapper;
			return NULL;
		}
		for (size_t i = 0; i < wrapper->fileWriteDatasLength; i++) {
			wrapper->fileWriteDatas[i].detectorChannel = params.detectorChannels[i];

			// Map the hardware channel (cast to uint8_t to safely handle negative channels) to the array index
			wrapper->channelToIndex[static_cast<uint8_t>(params.detectorChannels[i])] = i;

			wrapper->fileWriteDatas[i].bufferSizeElements = 2048;
			wrapper->fileWriteDatas[i].bufferElements = 0;
			wrapper->fileWriteDatas[i].buffer = (char*)malloc(wrapper->fileWriteDatas[i].bufferSizeElements * PACKED_SIZE);

			char filename[MAX_PATH];
			sprintf_s(filename, sizeof(filename), "%s\\data_%d.bin", directory, params.detectorChannels[i]);
			errno_t ret = fopen_s(&wrapper->fileWriteDatas[i].file, filename, "wb");
			if (ret) {
				for (size_t j = 0; j <= i; j++) {
					free(wrapper->fileWriteDatas[j].buffer);
					if (j != i) fclose(wrapper->fileWriteDatas[j].file);
				}
				free(wrapper->fileWriteDatas);
				delete wrapper;
				return NULL;
			}
		}
	}

	TimeTagger* castedTagger = static_cast<TimeTagger*>(tagger);

	//Delay laser channel by laser period in hardware for filtering purpose
	castedTagger->setDelayHardware(params.laserChannel, params.laserPeriod + params.hardwareDelayPs);

	//Set up conditional filter
	std::vector<channel_t> triggerVector(detectorChannelSet.begin(), detectorChannelSet.end());
	std::vector<channel_t> filterVector = { params.laserChannel };
	castedTagger->setConditionalFilter(triggerVector, filterVector);

	//Use software delay to push laser channel timestamps back forward
	castedTagger->setDelaySoftware(params.laserChannel, -params.laserPeriod);

	castedTagger->setTriggerLevel(params.laserChannel, params.laserTriggerVoltage);
	for (auto detectorChannel : detectorChannelSet) {
		castedTagger->setTriggerLevel(detectorChannel, params.detectorTriggerVoltage);
	}
	castedTagger->sync();

	wrapper->measurement = new TDMeasurement(castedTagger, params.laserChannel, detectorChannelSet, params.laserPeriod);

	if (wrapper->enableFileWrite) {
		// Pre-allocate initial vectors to prevent heap pauses during early execution
		for (int i = 0; i < 3; i++) {
			std::vector<MacroMicro_t> v;
			v.reserve(20000000);
			wrapper->recycleQueue.push(std::move(v));
		}
		wrapper->diskThreadRunning = true;
		wrapper->diskThread = std::thread(fileWriterWorker, wrapper);
	}
	else {
		wrapper->noFileBuffer.reserve(20000000);
	}

	return wrapper;
}

void freeMeasurement(void* obj) {
	MeasurementWrapper* wrapper = static_cast<MeasurementWrapper*>(obj);

	if (wrapper->enableFileWrite) {
		wrapper->diskThreadRunning = false;
		wrapper->diskCV.notify_all();
		if (wrapper->diskThread.joinable()) {
			wrapper->diskThread.join();
		}
		for (size_t i = 0; i < wrapper->fileWriteDatasLength; i++) {
			fclose(wrapper->fileWriteDatas[i].file);
			free(wrapper->fileWriteDatas[i].buffer);
		}
		free(wrapper->fileWriteDatas);
	}

	delete wrapper->measurement;
	delete wrapper;
}

void startMeasurement(void* obj) {
	static_cast<MeasurementWrapper*>(obj)->measurement->start();
}

void stopMeasurement(void* obj) {
	static_cast<MeasurementWrapper*>(obj)->measurement->stop();
}

int getData(void* obj, MacroMicro_t* outputData, size_t maxOutputSize, size_t* actualOutputSize, int activeChannel) {
	MeasurementWrapper* wrapper = static_cast<MeasurementWrapper*>(obj);

	std::vector<MacroMicro_t> incomingBatch;
	if (wrapper->enableFileWrite) {
		std::lock_guard<std::mutex> lock(wrapper->diskMutex);
		if (!wrapper->recycleQueue.empty()) {
			incomingBatch = std::move(wrapper->recycleQueue.front());
			wrapper->recycleQueue.pop();
		}
	}
	else {
		incomingBatch = std::move(wrapper->noFileBuffer);
	}

	// Safety fallback if queue was empty
	if (incomingBatch.capacity() == 0) incomingBatch.reserve(20000000);

	bool error = wrapper->measurement->getData(incomingBatch);
	if (error) return 2;

	size_t copiedCount = 0;
	for (const auto& d : incomingBatch) {
		if (d.channel == activeChannel || d.channel == -activeChannel) {
			if (copiedCount < maxOutputSize) {
				outputData[copiedCount] = d;
				copiedCount++;
			}
			else break;
		}
	}
	*actualOutputSize = copiedCount;

	if (wrapper->enableFileWrite) {
		if (!incomingBatch.empty()) {
			std::lock_guard<std::mutex> lock(wrapper->diskMutex);
			wrapper->diskQueue.push(std::move(incomingBatch));
			wrapper->diskCV.notify_one();
		}
		else {
			// Return unused vector to recycle queue immediately
			std::lock_guard<std::mutex> lock(wrapper->diskMutex);
			wrapper->recycleQueue.push(std::move(incomingBatch));
		}
	}
	else {
		// Return capacity to non-saving persistent buffer
		incomingBatch.clear();
		wrapper->noFileBuffer = std::move(incomingBatch);
	}

	return 0;
}