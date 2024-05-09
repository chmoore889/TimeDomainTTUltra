#include "TDMeasurement.h"
#include "ExternalInterface.h"
#include "windows.h"

#define member_size(type, member) (sizeof( ((type *)0)->member ))
#define PACKED_SIZE (member_size(MacroMicro_t, macroTime) + member_size(MacroMicro_t, microTime))

static bool enableFileWrite = false;
static char outputDir[MAX_PATH];

typedef struct {
	FILE* file;
	size_t bufferSizeElements;
	size_t bufferElements;
	char* buffer;
	int detectorChannel;
} FileWriteData;

static int channelIndexOf(FileWriteData* channelData, size_t length, int channel) {
	for (int i = 0; i < length; i++) {
		if (channelData[i].detectorChannel == channel) {
			return i;
		}
	}
	return -1;
}

static FileWriteData* fileWriteDatas;
static size_t fileWriteDatasLength;

void* getTagger() {
	TimeTagger* tagger;
	try {
		tagger = createTimeTagger();
		return tagger;
	}
	catch (...) {
		return NULL;
	}
}

void freeTagger(void* tagger) {
	TimeTagger* castedTagger = static_cast<TimeTagger*>(tagger);
	freeTimeTagger(castedTagger);
}

void* newMeasurement(void* tagger, MeasurementParams_t params, const char* directory) {
	if(directory == NULL) {
		enableFileWrite = false;
	} else {
		enableFileWrite = true;
		const errno_t err = strcpy_s(outputDir, sizeof(outputDir), directory);
		if (err) {
			return NULL;
		}
	}

	std::set<channel_t> detectorChannelSet;
	for (size_t x = 0; x < params.detectorChannelsLength; x++) {
		detectorChannelSet.insert(params.detectorChannels[x]);
	}

	//Generate buffers for file writing
	if (enableFileWrite) {
		fileWriteDatasLength = params.detectorChannelsLength;
		fileWriteDatas = (FileWriteData*) malloc(fileWriteDatasLength * sizeof(*fileWriteDatas));
		if (fileWriteDatas == NULL) {
			return NULL;
		}
		for(size_t i = 0; i < fileWriteDatasLength; i++) {
			fileWriteDatas[i].detectorChannel = params.detectorChannels[i];
			fileWriteDatas[i].bufferSizeElements = 2048;
			fileWriteDatas[i].bufferElements = 0;
			fileWriteDatas[i].buffer = (char*) malloc(fileWriteDatas[i].bufferSizeElements * PACKED_SIZE);
			if (fileWriteDatas[i].buffer == NULL) {
				for (size_t j = 0; j < i; j++) {
					free(fileWriteDatas[i].buffer);
				}
				return NULL;
			}
			char filename[MAX_PATH];
			sprintf_s(filename, sizeof(filename), "%s\\data_%d.bin", directory, params.detectorChannels[i]);
			errno_t ret = fopen_s(&fileWriteDatas[i].file, filename, "wb");
			if (ret) {
				printf("Error opening file %s\n", filename);
				for (size_t j = 0; j <= i; j++) {
					free(fileWriteDatas[i].buffer);
					if (j != i) {
						fclose(fileWriteDatas[i].file);
					}
				}
				return NULL;
			}
		}
	}

	TimeTagger* castedTagger = static_cast<TimeTagger*>(tagger);

	//Setup hardware params
	std::vector<channel_t> triggerVector(detectorChannelSet.begin(), detectorChannelSet.end());
	std::vector<channel_t> filterVector = { params.laserChannel };
	castedTagger->setConditionalFilter(triggerVector, filterVector);

	castedTagger->setTriggerLevel(params.laserChannel, params.laserTriggerVoltage);
	for (auto detectorChannel : detectorChannelSet) {
		castedTagger->setTriggerLevel(detectorChannel, params.detectorTriggerVoltage);
	}

	castedTagger->sync();

	return new TDMeasurement(
		castedTagger,
		params.laserChannel,
		detectorChannelSet,
		params.laserPeriod
	);
}

void freeMeasurement(void* obj) {
	TDMeasurement* castedMeasurement = static_cast<TDMeasurement*>(obj);
	delete castedMeasurement;

	if (enableFileWrite) {
		for (size_t i = 0; i < fileWriteDatasLength; i++) {
			fclose(fileWriteDatas[i].file);
			free(fileWriteDatas[i].buffer);
		}
		free(fileWriteDatas);
	}
}

void startMeasurement(void* obj) {
	TDMeasurement* castedMeasurement = static_cast<TDMeasurement*>(obj);
	castedMeasurement->start();
}

void stopMeasurement(void* obj) {
	TDMeasurement* castedMeasurement = static_cast<TDMeasurement*>(obj);
	castedMeasurement->stop();
}

int getData(void* obj, MacroMicro_t** outputData, size_t* outputDataSize) {
	TDMeasurement* castedMeasurement = static_cast<TDMeasurement*>(obj);
	std::pair<std::vector<MacroMicro_t>, bool> dataRaw = castedMeasurement->getData();

	if(dataRaw.second) {
		return 2;
	}

	//Set size and return early if no data
	*outputDataSize = dataRaw.first.size();
	if (*outputDataSize == 0) {
		*outputData = NULL;
		return 0;
	}

	//Copy to heap allocated array
	*outputData = (MacroMicro_t*)malloc(*outputDataSize * sizeof(**outputData));
	if (*outputData == NULL) {
		return 1;
	}

	std::copy(dataRaw.first.begin(), dataRaw.first.end(), *outputData);

	//Write to file
	if (enableFileWrite) {
		for (int x = 0; x < *outputDataSize; x++) {
			const MacroMicro_t d = (*outputData)[x];
			int channelIndex = channelIndexOf(fileWriteDatas, fileWriteDatasLength, d.channel);
			if (channelIndex < 0) {
				printf("Error finding channel index\n");
				return 1;
			}
			if (fileWriteDatas[channelIndex].bufferElements == fileWriteDatas[channelIndex].bufferSizeElements) {
				char* temp = (char*) realloc(fileWriteDatas[channelIndex].buffer, 2 * fileWriteDatas[channelIndex].bufferSizeElements * PACKED_SIZE);
				if (temp == NULL) {
					printf("Error reallocating buffer\n");
					return 1;
				}
				fileWriteDatas[channelIndex].buffer = temp;
				fileWriteDatas[channelIndex].bufferSizeElements = 2 * fileWriteDatas[channelIndex].bufferSizeElements;
			}
			char* buffer = fileWriteDatas[channelIndex].buffer + fileWriteDatas[channelIndex].bufferElements * PACKED_SIZE;
			memcpy(buffer, &d.macroTime, sizeof(d.macroTime));
			memcpy(buffer + sizeof(d.macroTime), &d.microTime, sizeof(d.microTime));
			fileWriteDatas[channelIndex].bufferElements++;
		}

		for (int x = 0; x < fileWriteDatasLength; x++) {
			fwrite(fileWriteDatas[x].buffer, PACKED_SIZE, fileWriteDatas[x].bufferElements, fileWriteDatas[x].file);
			fileWriteDatas[x].bufferElements = 0;
		}
	}

	return 0;
}