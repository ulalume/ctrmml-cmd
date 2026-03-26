#include "highlight_tracker.h"

#include "input.h"

#include <unordered_set>

TrackInfoGenerator::TrackInfoGenerator(Song &song, Track &track)
		: Player(song, track), TrackInfo(), slur_flag(false)
{
	loop_start = -1;
	loop_length = 0;
	length = 0;

	while (is_enabled())
		step_event();

	length = get_play_time();
	if (loop_start >= 0)
		loop_length = length - loop_start;
}

void TrackInfoGenerator::write_event()
{
	if ((event.type == Event::NOTE || event.type == Event::TIE || event.type == Event::REST) &&
			(on_time || off_time))
	{
		ExtEvent ext;
		ext.note = event.param;
		ext.on_time = on_time;
		ext.is_tie = (event.type == Event::TIE);
		ext.is_slur = slur_flag;
		ext.off_time = off_time;
		ext.volume = get_var(Event::VOL_FINE);
		ext.coarse_volume_flag = coarse_volume_flag();
		ext.instrument = get_var(Event::INS);
		ext.transpose = get_var(Event::TRANSPOSE);
		ext.pitch_envelope = get_var(Event::PITCH_ENVELOPE);
		ext.portamento = get_var(Event::PORTAMENTO);
		ext.references = get_references();
		if (get_var(Event::DRUM_MODE))
			ext.references.push_back(reference);
		events.emplace(get_play_time(), std::move(ext));

		slur_flag = false;
	}
	else if (event.type == Event::SEGNO)
	{
		loop_start = get_play_time();
	}
	else if (event.type == Event::SLUR)
	{
		slur_flag = true;
	}
}

bool TrackInfoGenerator::loop_hook()
{
	return false;
}

std::vector<HighlightPosition> collect_highlights(
		const std::map<int, TrackInfo> &tracks,
		uint32_t ticks,
		uint32_t max_entries)
{
	std::unordered_set<uint64_t> uniq;
	for (const auto &track_pair : tracks)
	{
		const auto &info = track_pair.second;
		int offset = 0;

		if (ticks > static_cast<uint32_t>(info.length) && info.loop_length)
			offset = ((ticks - info.loop_start) / info.loop_length) * info.loop_length;

		auto it = info.events.lower_bound(ticks - offset);
		if (it != info.events.begin())
		{
			--it;
			const auto &ev = it->second;
			for (const auto &ref : ev.references)
			{
				if (!ref || ref->get_filename().size())
					continue;
				uint64_t key = (static_cast<uint64_t>(ref->get_line()) << 32) |
											 static_cast<uint64_t>(ref->get_column());
				uniq.insert(key);
			}
		}
	}

	std::vector<HighlightPosition> out;
	out.reserve(max_entries);
	for (uint64_t key : uniq)
	{
		if (out.size() >= max_entries)
			break;
		HighlightPosition pos{
				static_cast<uint32_t>(key >> 32),
				static_cast<uint32_t>(key & 0xffffffffu),
		};
		out.push_back(pos);
	}
	return out;
}

int32_t find_cursor_tick(
		const std::map<int, TrackInfo> &tracks,
		uint32_t line,
		uint32_t col)
{
	struct Candidate
	{
		bool found = false;
		uint32_t line = 0;
		uint32_t col = 0;
		uint32_t tick = 0;
	};

	Candidate same_line_before;
	Candidate same_line_after;
	Candidate later_lines;
	Candidate earlier_lines;

	for (const auto &track_pair : tracks)
	{
		const auto &info = track_pair.second;
		for (const auto &event_pair : info.events)
		{
			uint32_t tick = static_cast<uint32_t>(event_pair.first);
			const auto &ev = event_pair.second;
			for (const auto &ref : ev.references)
			{
				if (!ref || ref->get_filename().size())
					continue;

				uint32_t ref_line = ref->get_line();
				uint32_t ref_col = ref->get_column();

				if (ref_line == line && ref_col <= col)
				{
					if (!same_line_before.found ||
							ref_col > same_line_before.col ||
							(ref_col == same_line_before.col && tick < same_line_before.tick))
					{
						same_line_before = {true, ref_line, ref_col, tick};
					}
					continue;
				}

				if (ref_line == line && ref_col > col)
				{
					if (!same_line_after.found ||
							ref_col < same_line_after.col ||
							(ref_col == same_line_after.col && tick < same_line_after.tick))
					{
						same_line_after = {true, ref_line, ref_col, tick};
					}
					continue;
				}

				if (ref_line > line)
				{
					if (!later_lines.found ||
							ref_line < later_lines.line ||
							(ref_line == later_lines.line && ref_col < later_lines.col) ||
							(ref_line == later_lines.line && ref_col == later_lines.col &&
							 tick < later_lines.tick))
					{
						later_lines = {true, ref_line, ref_col, tick};
					}
					continue;
				}

				if (!earlier_lines.found ||
						ref_line > earlier_lines.line ||
						(ref_line == earlier_lines.line && ref_col > earlier_lines.col) ||
						(ref_line == earlier_lines.line && ref_col == earlier_lines.col &&
						 tick < earlier_lines.tick))
				{
					earlier_lines = {true, ref_line, ref_col, tick};
				}
			}
		}
	}

	if (same_line_before.found)
		return static_cast<int32_t>(same_line_before.tick);
	if (same_line_after.found)
		return static_cast<int32_t>(same_line_after.tick);
	if (later_lines.found)
		return static_cast<int32_t>(later_lines.tick);
	if (earlier_lines.found)
		return static_cast<int32_t>(earlier_lines.tick);

	return -1;
}

uint32_t find_start_ticks(
		const std::map<int, TrackInfo> &tracks,
		uint32_t line,
		uint32_t col)
{
	uint32_t best = 0;
	bool found = false;

	for (const auto &track_pair : tracks)
	{
		const auto &info = track_pair.second;
		for (const auto &event_pair : info.events)
		{
			uint32_t tick = static_cast<uint32_t>(event_pair.first);
			const auto &ev = event_pair.second;
			for (const auto &ref : ev.references)
			{
				if (!ref || ref->get_filename().size())
					continue;
				uint32_t ref_line = ref->get_line();
				uint32_t ref_col = ref->get_column();
				if (ref_line > line || (ref_line == line && ref_col >= col))
				{
					if (!found || tick < best)
					{
						best = tick;
						found = true;
					}
				}
			}
		}
	}

	return found ? best : 0;
}
