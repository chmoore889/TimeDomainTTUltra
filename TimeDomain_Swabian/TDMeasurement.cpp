#include <cassert>
#include <stdio.h>
#include "TDMeasurement.h"

TDMeasurement::TDMeasurement(TimeTaggerBase* tagger, channel_t laser_channel, std::set<channel_t> detector_channels,
    __int16 laserPeriod)
    : IteratorBase(tagger), laser_channel(laser_channel), detector_channels(detector_channels), laserPeriod(laserPeriod) {

    if (detector_channels.size() == 0) {
        throw std::invalid_argument("detector_channels cannot be empty");
    }

    registerChannel(laser_channel);
    for (auto detector_channel : detector_channels) {
        registerChannel(detector_channel);
    }

    clear_impl();

    // Map the set to a flat array for O(1) lookups
    is_detector_channel.resize(256, false);
    for (auto detector_channel : detector_channels) {
        is_detector_channel[static_cast<uint8_t>(detector_channel)] = true;
    }

    //Reserve lots of space for photons
    data.reserve(20000000);

    finishInitialization();
}

TDMeasurement::~TDMeasurement() {
    // This measurement must be stopped before deconstruction. This will wait until no thread is within next_impl.
    stop();
}

bool TDMeasurement::getData(std::vector<MacroMicro_t>& out_vector) {
    bool flagState;

    {
        auto lk = getLock();

        // O(1) swap: out_vector gets the data, 'data' inherits out_vector's massive capacity
        out_vector.swap(data);

        flagState = errorFlag;
    }

    return flagState;
}

void TDMeasurement::clear_impl() {
    errorFlag = false;
}

void TDMeasurement::on_start() {
    clear_impl();
}

void TDMeasurement::on_stop() {
    // optional callback
}

// Here we handle the incoming time-tags.
bool TDMeasurement::next_impl(std::vector<Tag>& incoming_tags, timestamp_t begin_time, timestamp_t end_time) {
    // iterate over all the tags received
    //printf("incoming tags length: %zu\n", incoming_tags.size());
    for (const Tag& tag : incoming_tags) {
        switch (tag.type) {
        case Tag::Type::Error:         // happens on clock switches and USB errors
        case Tag::Type::OverflowBegin: // indicates the begin of overflows
        case Tag::Type::OverflowEnd:   // indicates the end of overflows
        case Tag::Type::MissedEvents:  // reports the amount of tags in overflow
            //printf("Overflow\n");
            errorFlag = true;
            break;

        case Tag::Type::TimeTag:
            //printf("channel %i\n", tag.channel);
            if (tag.channel == laser_channel) {
                last_laser_time = tag.time;
            }
            //If not the laser channel, it must be detector
            else {
                assert(is_detector_channel[static_cast<uint8_t>(tag.channel)]);

                timestamp_t microTime = (tag.time - last_laser_time) % laserPeriod;

                // Ensure we've seen at least one laser tag before processing detector events
                if (last_laser_time != 0) {
                    data.push_back(MacroMicro_t{
                        static_cast<__int8>(tag.channel),                 // channel
                        static_cast<long long>(tag.time),                 // macroTime
                        static_cast<__int16>(microTime)  // microTime
                        });
                }
            }
            break;
        }
    }

    // return true if incoming_tags was modified. If so, please keep care about the requirements:
    // -- all tags must be sorted
    // -- begin_time <= tags < end_time
    return false;
}
