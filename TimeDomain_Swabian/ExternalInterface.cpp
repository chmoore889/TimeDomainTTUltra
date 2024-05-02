#include "TDMeasurement.h"
#include "ExternalInterface.h"

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

void* newMeasurement(void* tagger, MeasurementParams_t params) {
	std::set<channel_t> detectorChannelSet;
	for (size_t x = 0; x < params.detectorChannelsLength; x++) {
		detectorChannelSet.insert(params.detectorChannels[x]);
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
	return 0;
}