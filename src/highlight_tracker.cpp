#include "highlight_tracker.h"

#include "input.h"

#include <algorithm>
#include <limits>
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

namespace
{
	constexpr int kMaxChannels = 16;

	struct EditorTickInfo
	{
		uint32_t line_tick = 0;
		uint32_t cursor_tick = 0;
	};

	bool is_note_or_jump(Event::Type type)
	{
		return type == Event::NOTE || type == Event::TIE ||
					 type == Event::REST || type == Event::JUMP;
	}

	bool is_loop_event(Event::Type type)
	{
		return type == Event::LOOP_START || type == Event::LOOP_BREAK ||
					 type == Event::LOOP_END;
	}

	uint32_t get_subroutine_length(
			Song &song,
			unsigned int param,
			unsigned int max_recursion);

	uint32_t get_loop_length(
			Song &song,
			Track &track,
			unsigned long position,
			unsigned int max_recursion)
	{
		if (max_recursion == 0)
			return 0;

		int depth = 0;
		int count = track.get_event(position).param - 1;
		int start_time = 0;
		int end_time = track.get_event(position).play_time;
		int break_time = 0;
		while (position-- > 0)
		{
			auto &event = track.get_event(position);
			start_time = event.play_time;
			if (event.type == Event::LOOP_END)
			{
				depth++;
			}
			else if (event.type == Event::LOOP_BREAK && !depth)
			{
				break_time = end_time - event.play_time;
			}
			else if (event.type == Event::LOOP_START)
			{
				if (depth)
					depth--;
				else
					break;
			}
		}
		return static_cast<uint32_t>((end_time - start_time) * count - break_time);
	}

	uint32_t get_subroutine_length(
			Song &song,
			unsigned int param,
			unsigned int max_recursion)
	{
		if (max_recursion == 0)
			return 0;

		try
		{
			Track &track = song.get_track(param);
			auto event_count = track.get_event_count();
			if (!event_count)
				return 0;

			auto &event = track.get_event(event_count - 1);
			uint32_t end_time = event.play_time + event.on_time + event.off_time;
			if (event.type == Event::JUMP)
			{
				end_time = event.play_time +
									 get_subroutine_length(song, event.param, max_recursion - 1);
			}
			else if (event.type == Event::LOOP_END)
			{
				end_time = event.play_time +
									 get_loop_length(song, track, event_count - 1, max_recursion - 1);
			}
			return end_time - track.get_event(0).play_time;
		}
		catch (std::exception &)
		{
			return 0;
		}
	}

	EditorTickInfo find_editor_ticks(
			Song &song,
			const CompileLineMap &lines,
			uint32_t line,
			uint32_t col)
	{
		EditorTickInfo info{};
		uint32_t song_pos_at_line = std::numeric_limits<uint32_t>::max();
		uint32_t song_pos_at_cursor = std::numeric_limits<uint32_t>::max();

		auto line_it = lines.find(static_cast<int>(line));
		if (line_it == lines.end())
			return info;

		const auto &line_map = line_it->second;
		for (const auto &track_pos : line_map)
		{
			if (track_pos.first >= kMaxChannels)
				continue;

			try
			{
				Track &track = song.get_track(track_pos.first);
				unsigned long position = track_pos.second;
				unsigned long event_count = track.get_event_count();
				if (!event_count)
					continue;

				while (position-- > 0)
				{
					auto ref = track.get_event(position).reference;
					if (ref != nullptr)
					{
						int ref_line = ref->get_line();
						int ref_col = ref->get_column();
						if (ref_line < static_cast<int>(line) ||
								(ref_line == static_cast<int>(line) &&
								 ref_col < static_cast<int>(col)))
						{
							break;
						}
					}
				}

				bool found_right_of_cursor = false;
				while (++position < event_count)
				{
					auto &event = track.get_event(position);
					if (is_note_or_jump(event.type))
					{
						song_pos_at_cursor = std::min(song_pos_at_cursor, event.play_time);
						found_right_of_cursor = true;
						break;
					}
					if (is_loop_event(event.type))
						break;
				}

				bool passed_line = false;
				while ((!passed_line || !found_right_of_cursor) && position-- > 0)
				{
					auto &event = track.get_event(position);
					uint32_t length = event.on_time + event.off_time;
					if (event.type == Event::JUMP)
					{
						length = get_subroutine_length(song, event.param, 10);
					}
					else if (event.type == Event::LOOP_END)
					{
						length = get_loop_length(song, track, position, 10);
					}

					auto ref = event.reference;
					if (ref != nullptr)
					{
						passed_line = ref->get_line() < static_cast<int>(line);
						if (!passed_line)
						{
							song_pos_at_line = std::min(song_pos_at_line, event.play_time);
						}
					}

					if (!found_right_of_cursor && length != 0)
					{
						if (is_note_or_jump(event.type) || event.type == Event::LOOP_END)
						{
							song_pos_at_cursor =
									std::min(song_pos_at_cursor, event.play_time + length);
						}
					}
				}

				if (song_pos_at_cursor < song_pos_at_line)
					song_pos_at_line = song_pos_at_cursor;
			}
			catch (std::exception &)
			{
				continue;
			}
		}

		if (song_pos_at_line != std::numeric_limits<uint32_t>::max())
			info.line_tick = song_pos_at_line;
		if (song_pos_at_cursor != std::numeric_limits<uint32_t>::max())
			info.cursor_tick = song_pos_at_cursor;
		return info;
	}
}

int32_t find_cursor_tick(
		const Song &song,
		const CompileLineMap &lines,
		uint32_t line,
		uint32_t col)
{
	return static_cast<int32_t>(
			find_editor_ticks(const_cast<Song &>(song), lines, line, col).cursor_tick);
}

uint32_t find_line_tick(
		const Song &song,
		const CompileLineMap &lines,
		uint32_t line,
		uint32_t col)
{
	return find_editor_ticks(const_cast<Song &>(song), lines, line, col).line_tick;
}

uint32_t find_start_ticks(
		const Song &song,
		const CompileLineMap &lines,
		uint32_t line,
		uint32_t col)
{
	return find_editor_ticks(const_cast<Song &>(song), lines, line, col).cursor_tick;
}
