#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "conio.h"
#include "ExternalInterface.h"
#include "windows.h"

bool iskeypressed(unsigned timeout_ms) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD startTime = GetTickCount();

    while (true) {
        DWORD elapsed = GetTickCount() - startTime;
        if (elapsed >= timeout_ms) {
            return false;
        }

        DWORD waitRes = WaitForSingleObject(hIn, timeout_ms - elapsed);
        if (waitRes != WAIT_OBJECT_0) {
            return false;
        }

        INPUT_RECORD record;
        DWORD eventsRead = 0;
        if (!ReadConsoleInput(hIn, &record, 1, &eventsRead) || eventsRead == 0) {
            return false;
        }

        // Return true only on an actual key-down event
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
            return true;
        }

        // Other events (mouse moves, key releases, focus) are consumed and ignored
    }
}

int main() {
    void* tagger = getTagger();
    printf("Got Time Tagger %p\n", tagger);
    if (tagger == NULL) {
        return 1;
    }

    int channels[] = { 1 };

    void* measurement = newMeasurement(tagger, (MeasurementParams_t) {
        .laserChannel = -2,
            .laserTriggerVoltage = -0.5,
            .laserPeriod = 12500,
            .detectorChannels = channels,
            .detectorChannelsLength = sizeof(channels) / sizeof(*channels),
            .detectorTriggerVoltage = 0.9,
    }, "C:\\Users\\sunar\\Desktop");

    if (measurement == NULL) {
        freeTagger(tagger);
        return 1;
    }

    printf("Starting measurement\n");
    //startMeasurement(measurement);
    //printf("Measurement started\n");

    // 1. Allocate a persistent buffer outside the read loop
    size_t maxDataSize = 1000000;
    MacroMicro_t* dataBuffer = (MacroMicro_t*)malloc(maxDataSize * sizeof(MacroMicro_t));
    if (dataBuffer == NULL) {
        printf("Error: Failed to allocate data buffer\n");
        freeMeasurement(measurement);
        freeTagger(tagger);
        return 1;
    }

    // 2. Define the active channel to filter against
    int activeChannel = 1;

    while (!iskeypressed(50)) {
        size_t actualDataSize = 0;

        // 3. Call the updated API signature
        int ret = getData(measurement, dataBuffer, maxDataSize, &actualDataSize, activeChannel);

        if (ret) {
            printf("Error getting data\n");
            break;
        }
        else if (actualDataSize > 0) {
            printf("Got data %zu\n", actualDataSize);
        }
    }

    printf("Stopping measurement\n");
    stopMeasurement(measurement);
    freeMeasurement(measurement);

    // 4. Free the persistent buffer
    free(dataBuffer);

    printf("Freeing taggers\n");
    freeTagger(tagger);
    return 0;
}