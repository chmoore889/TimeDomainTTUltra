#pragma once

#include <vector>
#include <set>
#include "TimeTagger.h"
#include "ExternalInterface.h"

class TDMeasurement : public IteratorBase {
public:
	TDMeasurement(TimeTaggerBase* tagger, channel_t laser_channel, std::set<channel_t> detector_channels,
		__int16 laserPeriod);

	~TDMeasurement();

	std::vector<MacroMicro_t> getData();

protected:
	/**
	 * next receives and handles the incoming tags.
	 * All tags are sorted by time and comply with begin_time <= tag.time < end_time and end_time shall be the begin_time
	 * of the next invocation. incoming_tags can be modified to filter or inject tags.
	 * \param incoming_tags vector of incoming tags to process, might be empty
	 * \param begin_time    begin timestamp of this block of tags
	 * \param end_time      end timestamp of this block of tags
	 * \return true if the content of this block was modified, false otherwise
	 * \note This method is called with the measurement mutex locked.
	 */
	bool next_impl(std::vector<Tag>& incoming_tags, timestamp_t begin_time, timestamp_t end_time) override;

	/**
	 * reset measurement
	 * \note This method is called with the measurement mutex locked.
	 */
	void clear_impl() override;

	/**
	 * callback before the measurement is started
	 * \note This method is called with the measurement mutex locked.
	 */
	void on_start() override;

	/**
	 * callback after the measurement is stopped
	 * \note This method is called with the measurement mutex locked.
	 */
	void on_stop() override;

private:
	// channels used for this measurement
	const channel_t laser_channel;
	const std::set<channel_t> detector_channels;
	const timestamp_t laserPeriod;

	std::vector<MacroMicro_t> data;

	// data variable bin_index -> counts
	std::vector<std::pair<__int8, timestamp_t>> unprocessedTags;
	bool errorFlag = false;
};
