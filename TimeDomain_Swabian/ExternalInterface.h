#pragma once

#include "HelperStructs.h"

#ifdef TIMEDOMAINSWABIAN_EXPORTS
#define TD_API __declspec(dllexport)
#else
#define TD_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif
	TD_API void* getTagger();
	TD_API void freeTagger(void* tagger);

	TD_API void* newMeasurement(void* tagger, MeasurementParams_t params);
	TD_API void freeMeasurement(void* obj);
	TD_API void startMeasurement(void* obj);
	TD_API void stopMeasurement(void* obj);
	TD_API int getData(void* obj, MacroMicro_t** outputData, size_t* outputDataSize);

#ifdef __cplusplus
}
#endif