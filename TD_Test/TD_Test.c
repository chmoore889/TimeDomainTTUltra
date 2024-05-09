#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "conio.h"
#include "ExternalInterface.h"

#include "windows.h"

bool iskeypressed(unsigned timeout_ms) {
    return WaitForSingleObject(
        GetStdHandle(STD_INPUT_HANDLE),
        timeout_ms
    ) == WAIT_OBJECT_0;
}

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
    }, "C:\\Users\\Christopher\\Desktop");
    if (measurement == NULL) {
        freeTagger(tagger);
        return 1;
    }

    printf("Starting measurement\n");
    //startMeasurement(measurement);
    //printf("Measurement started\n");

    while (!iskeypressed(50)) {
        MacroMicro_t* data;
        size_t dataSize;
        int ret = getData(measurement, &data, &dataSize);
        if(ret) {
			printf("Error getting data\n");
			break;
        }
        else if (dataSize) {
            printf("Got data %zu\n", dataSize);
        }
        free(data);
    }

    printf("Stopping measurement\n");
    stopMeasurement(measurement);
    freeMeasurement(measurement);

    printf("Freeing taggers\n");

    freeTagger(tagger);
    return 0;
}