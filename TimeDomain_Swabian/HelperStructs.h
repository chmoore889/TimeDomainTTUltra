#pragma once

typedef struct
{
	__int8 channel;
	long long macroTime;
	__int16 microTime;
} MacroMicro_t;

typedef struct
{
	int laserChannel;
	__int16 laserPeriod;
	double laserTriggerVoltage;


	int* detectorChannels;
	size_t detectorChannelsLength;
	double detectorTriggerVoltage;
} MeasurementParams_t;