#include <stdio.h>
#include <stdlib.h>
#include "conio.h"
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
    
    while (1) {
        MacroMicro_t* data;
        size_t dataSize;
        int ret = getData(measurement, &data, &dataSize);
        free(data);
        if(ret) {
			printf("Error getting data\n");
			break;
        }
        else if (dataSize) {
            printf("Got data %zu\n", dataSize);
        }
    }

    printf("Stopping measurement\n");
    stopMeasurement(measurement);
    freeMeasurement(measurement);

    printf("Freeing taggers\n");

    freeTagger(tagger);
    return 0;
}