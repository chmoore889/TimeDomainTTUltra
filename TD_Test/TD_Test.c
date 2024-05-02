#include <stdio.h>
#include "ExternalInterface.h"

int main() {
    void* tagger = getTagger();
    printf("Got Time Tagger %p\n", tagger);
    if (tagger == NULL) {
        return 1;
    }

    int channels[] = {2};
    void* measurement = newMeasurement(tagger, (MeasurementParams_t) {
        .laserChannel = -1,
        .laserTriggerVoltage = -0.5,
        .laserPeriod = 12500,
        .detectorChannels = channels,
        .detectorChannelsLength = sizeof(channels) / sizeof(*channels),
        .detectorTriggerVoltage = 0.9,
    });
    if (measurement == NULL) {
        freeTagger(tagger);
        return 1;
    }

    printf("Starting measurement\n");
    startMeasurement(measurement);
    printf("Measurement started\n");
    
    while (getchar() != '\n') {
        MacroMicro_t* data;
        size_t dataSize;
        int ret = getData(measurement, &data, &dataSize);
        if(ret != 0) {
			printf("Error getting data\n");
			break;
		}
    }

    printf("Stopping measurement\n");
    stopMeasurement(measurement);

    printf("Freeing taggers\n");

    freeTagger(tagger);
    return 0;
}