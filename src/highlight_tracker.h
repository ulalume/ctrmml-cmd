#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "player.h"
#include "track.h"
#include "song.h"

struct HighlightPosition
{
	uint32_t line;
	uint32_t col;
};

struct TrackInfo
{
	struct ExtEvent
	{
		uint16_t note;
		uint16_t on_time;
		bool is_tie;
		bool is_slur;
		uint16_t off_time;
		bool coarse_volume_flag;
		uint16_t volume;
		uint16_t instrument;
		int16_t transpose;
		uint16_t pitch_envelope;
		uint16_t portamento;
		std::vector<std::shared_ptr<InputRef>> references;
	};

	std::map<int, ExtEvent> events;
	int loop_start;
	unsigned int loop_length;
	unsigned int length;
};

class TrackInfoGenerator : public Player, public TrackInfo
{
public:
	TrackInfoGenerator(Song &song, Track &track);

private:
	void write_event() override;
	bool loop_hook() override;

	bool slur_flag;
};

std::vector<HighlightPosition> collect_highlights(
		const std::map<int, TrackInfo> &tracks,
		uint32_t ticks,
		uint32_t max_entries);

uint32_t find_start_ticks(
		const std::map<int, TrackInfo> &tracks,
		uint32_t line,
		uint32_t col);
