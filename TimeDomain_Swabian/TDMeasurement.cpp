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

    //Reserve space for a few detected photons per laser period
    unprocessedTags.reserve(detector_channels.size() * 8);

    //Reserve lots of space for photons
    data.reserve(20000000);

    finishInitialization();
}

TDMeasurement::~TDMeasurement() {
    // This measurement must be stopped before deconstruction. This will wait until no thread is within next_impl.
    stop();
}

// Get data copies the local data to a newly allocated memory.
std::pair<std::vector<MacroMicro_t>, bool> TDMeasurement::getData() {
    // This lock object will ensure that no other thread is within next_impl.
    auto lk = getLock();

    const std::vector<MacroMicro_t> toReturn = data;
    data.clear();

    return std::make_pair(toReturn, errorFlag);
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
                data.reserve(data.size() + unprocessedTags.size());
                for (auto detTime : unprocessedTags) {
                    const MacroMicro_t toPush = {
                        detTime.first,//channel
                        detTime.second,//macro
                        static_cast<__int16>(laserPeriod - (tag.time - detTime.second))//micro
                    };
                    data.push_back(toPush);
                }
                unprocessedTags.clear();
            }
            //If not the laser channel, it must be detector
            else {
                assert(detector_channels.find(tag.channel) != detector_channels.end());

                unprocessedTags.push_back(std::make_pair(tag.channel, tag.time));
            }
            break;
        }
    }

    // return true if incoming_tags was modified. If so, please keep care about the requirements:
    // -- all tags must be sorted
    // -- begin_time <= tags < end_time
    return false;
}
