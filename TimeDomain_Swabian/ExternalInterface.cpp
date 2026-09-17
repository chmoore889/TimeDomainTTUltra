#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <iostream>

#include "TDMeasurement.h"
#include "ExternalInterface.h"
#include "windows.h"

#define ENABLE_COMPRESSION // Comment this out to disable compression

#ifdef ENABLE_COMPRESSION
#include <zlib.h>
typedef gzFile WriteFileHandle;
#else
typedef FILE* WriteFileHandle;
#endif

typedef struct {
	WriteFileHandle macroFile;
	WriteFileHandle microFile;
	size_t bufferSizeElements;
	size_t bufferElements;
	long long* macroBuffer; // Typed buffer for macro times
	__int16* microBuffer;   // Typed buffer for micro times
	int detectorChannel;
	long long lastMacroTime;
} FileWriteData;

// 1. Thread State Encapsulation (Eliminates Static Globals)
struct MeasurementWrapper {
	TDMeasurement* measurement;
	bool enableFileWrite = false;
	FileWriteData* fileWriteDatas = nullptr;
	size_t fileWriteDatasLength = 0;

	// O(1) lookup table for file writer
	int channelToIndex[256];

	std::queue<std::vector<MacroMicro_t>> dartQueue; // <--- Changed from diskQueue
	std::queue<std::vector<MacroMicro_t>> recycleQueue;
	std::mutex dartMutex; // <--- Changed from diskMutex

	std::atomic<bool> workerThreadRunning{ false };
	std::thread workerThread;
	std::atomic<bool> hardwareError{ false }; // <--- Added to pass errors to Dart

	std::vector<MacroMicro_t> noFileBuffer; // Persistent buffer for non-saving runs
};

static void backgroundWorker(MeasurementWrapper* wrapper) {
	while (wrapper->workerThreadRunning) {
		std::vector<MacroMicro_t> batch;
		{
			std::lock_guard<std::mutex> lock(wrapper->dartMutex);
			if (!wrapper->recycleQueue.empty()) {
				batch = std::move(wrapper->recycleQueue.front());
				wrapper->recycleQueue.pop();
			}
		}

		// Fallback if Dart has all the buffers
		if (batch.capacity() == 0) batch.reserve(5000000);

		// 1. PULL DIRECTLY FROM HARDWARE
		bool error = wrapper->measurement->getData(batch);
		if (error) wrapper->hardwareError = true;

		if (batch.empty()) {
			// No new photons. Return buffer to recycle and sleep briefly.
			std::lock_guard<std::mutex> lock(wrapper->dartMutex);
			wrapper->recycleQueue.push(std::move(batch));
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			continue;
		}

		// 2. HIGHEST PRIORITY: DISK WRITING
		for (const auto& d : batch) {
			int channelIndex = wrapper->channelToIndex[static_cast<uint8_t>(d.channel)];
			if (channelIndex < 0) continue;

			if (wrapper->fileWriteDatas[channelIndex].bufferElements == wrapper->fileWriteDatas[channelIndex].bufferSizeElements) {
				size_t newSize = 2 * wrapper->fileWriteDatas[channelIndex].bufferSizeElements;
				long long* tempMacro = (long long*)realloc(wrapper->fileWriteDatas[channelIndex].macroBuffer, newSize * sizeof(long long));
				__int16* tempMicro = (__int16*)realloc(wrapper->fileWriteDatas[channelIndex].microBuffer, newSize * sizeof(__int16));

				if (tempMacro && tempMicro) {
					wrapper->fileWriteDatas[channelIndex].macroBuffer = tempMacro;
					wrapper->fileWriteDatas[channelIndex].microBuffer = tempMicro;
					wrapper->fileWriteDatas[channelIndex].bufferSizeElements = newSize;
				}
			}

			size_t currentIdx = wrapper->fileWriteDatas[channelIndex].bufferElements;
			long long currentMacro = d.macroTime;
			long long delta = currentMacro - wrapper->fileWriteDatas[channelIndex].lastMacroTime;
			wrapper->fileWriteDatas[channelIndex].lastMacroTime = currentMacro;

			wrapper->fileWriteDatas[channelIndex].macroBuffer[currentIdx] = delta;
			wrapper->fileWriteDatas[channelIndex].microBuffer[currentIdx] = d.microTime;
			wrapper->fileWriteDatas[channelIndex].bufferElements++;
		}

		for (size_t x = 0; x < wrapper->fileWriteDatasLength; x++) {
			size_t elements = wrapper->fileWriteDatas[x].bufferElements;
			if (elements > 0) {
#ifdef ENABLE_COMPRESSION
				gzwrite(wrapper->fileWriteDatas[x].macroFile, wrapper->fileWriteDatas[x].macroBuffer, (unsigned)(elements * sizeof(long long)));
				gzwrite(wrapper->fileWriteDatas[x].microFile, wrapper->fileWriteDatas[x].microBuffer, (unsigned)(elements * sizeof(__int16)));
#else
				fwrite(wrapper->fileWriteDatas[x].macroBuffer, sizeof(long long), elements, wrapper->fileWriteDatas[x].macroFile);
				fwrite(wrapper->fileWriteDatas[x].microBuffer, sizeof(__int16), elements, wrapper->fileWriteDatas[x].microFile);
#endif
				wrapper->fileWriteDatas[x].bufferElements = 0;
			}
		}

		// 3. LOW PRIORITY: PASS TO DART (With Data Dropping)
		{
			std::lock_guard<std::mutex> lock(wrapper->dartMutex);

			// If Dart is falling behind (more than 5 batches queued), drop the oldest batch
			if (wrapper->dartQueue.size() > 5) {
				wrapper->recycleQueue.push(std::move(wrapper->dartQueue.front()));
				wrapper->dartQueue.pop();
			}
			wrapper->dartQueue.push(std::move(batch));
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
			wrapper->channelToIndex[static_cast<uint8_t>(params.detectorChannels[i])] = i;

			wrapper->fileWriteDatas[i].bufferSizeElements = 2048;
			wrapper->fileWriteDatas[i].bufferElements = 0;

			// Allocate separated buffers
			wrapper->fileWriteDatas[i].macroBuffer = (long long*)malloc(wrapper->fileWriteDatas[i].bufferSizeElements * sizeof(long long));
			wrapper->fileWriteDatas[i].microBuffer = (__int16*)malloc(wrapper->fileWriteDatas[i].bufferSizeElements * sizeof(__int16));
			wrapper->fileWriteDatas[i].lastMacroTime = 0;

			char macroFilename[MAX_PATH];
			char microFilename[MAX_PATH];

#ifdef ENABLE_COMPRESSION
			sprintf_s(macroFilename, sizeof(macroFilename), "%s\\macroData_%d_compressed.bin", directory, params.detectorChannels[i]);
			sprintf_s(microFilename, sizeof(microFilename), "%s\\microData_%d_compressed.bin", directory, params.detectorChannels[i]);

			wrapper->fileWriteDatas[i].macroFile = gzopen(macroFilename, "wb");
			wrapper->fileWriteDatas[i].microFile = gzopen(microFilename, "wb");

			bool failed = (wrapper->fileWriteDatas[i].macroFile == NULL || wrapper->fileWriteDatas[i].microFile == NULL);
#else
			sprintf_s(macroFilename, sizeof(macroFilename), "%s\\macroData_%d.bin", directory, params.detectorChannels[i]);
			sprintf_s(microFilename, sizeof(microFilename), "%s\\microData_%d.bin", directory, params.detectorChannels[i]);

			errno_t ret1 = fopen_s(&wrapper->fileWriteDatas[i].macroFile, macroFilename, "wb");
			errno_t ret2 = fopen_s(&wrapper->fileWriteDatas[i].microFile, microFilename, "wb");

			bool failed = (ret1 != 0 || ret2 != 0);
#endif

			if (failed) {
				// Handle cleanup if file opening fails...
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
		wrapper->workerThreadRunning = true;
		wrapper->workerThread = std::thread(backgroundWorker, wrapper);
	}
	else {
		wrapper->noFileBuffer.reserve(20000000);
	}

	return wrapper;
}

void freeMeasurement(void* obj) {
	MeasurementWrapper* wrapper = static_cast<MeasurementWrapper*>(obj);

	if (wrapper->enableFileWrite) {
		wrapper->workerThreadRunning = false;
		if (wrapper->workerThread.joinable()) {
			wrapper->workerThread.join();
		}
		for (size_t i = 0; i < wrapper->fileWriteDatasLength; i++) {
#ifdef ENABLE_COMPRESSION
			gzclose(wrapper->fileWriteDatas[i].macroFile);
			gzclose(wrapper->fileWriteDatas[i].microFile);
#else
			fclose(wrapper->fileWriteDatas[i].macroFile);
			fclose(wrapper->fileWriteDatas[i].microFile);
#endif
			free(wrapper->fileWriteDatas[i].macroBuffer);
			free(wrapper->fileWriteDatas[i].microBuffer);
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

	if (wrapper->hardwareError) return 2;

	std::vector<MacroMicro_t> incomingBatch;

	if (wrapper->enableFileWrite) {
		std::lock_guard<std::mutex> lock(wrapper->dartMutex);
		if (wrapper->dartQueue.empty()) {
			*actualOutputSize = 0;
			return 0; // Dart will sleep and try again later
		}
		incomingBatch = std::move(wrapper->dartQueue.front());
		wrapper->dartQueue.pop();
	}
	else {
		// Fallback for when disk saving is disabled
		incomingBatch = std::move(wrapper->noFileBuffer);
		if (incomingBatch.capacity() == 0) incomingBatch.reserve(5000000);
		bool error = wrapper->measurement->getData(incomingBatch);
		if (error) return 2;
	}

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
		// Return vector to recycle queue immediately
		incomingBatch.clear();
		std::lock_guard<std::mutex> lock(wrapper->dartMutex);
		wrapper->recycleQueue.push(std::move(incomingBatch));
	}
	else {
		// Return capacity to non-saving persistent buffer
		incomingBatch.clear();
		wrapper->noFileBuffer = std::move(incomingBatch);
	}

	return 0;
}