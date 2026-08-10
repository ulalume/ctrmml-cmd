#include "check_note_range.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "input.h"
#include "player.h"
#include "song.h"
#include "track.h"

namespace
{
	constexpr int kMdsdrvRomNoteMin = 0;
	constexpr int kMdsdrvRomNoteExclusiveMax = 94; // SLR - NOTE = 0xe0 - 0x82
	constexpr int kMegadriveChannelTrackCount = 16;

	ctrmml_cmd::CheckMessage make_message_from_ref(
			const std::shared_ptr<InputRef> &ref,
			const std::string &display_name,
			const std::string &message,
			const std::string &code,
			uint32_t length = 1)
	{
		ctrmml_cmd::CheckMessage out{};
		out.message = message;
		out.code = code;
		out.length = length;
		if (ref)
		{
			out.path = ref->get_filename().empty() ? display_name : ref->get_filename();
			// InputRef stores 0-indexed line and 0-indexed column; JSON consumers expect 1-indexed
			out.line = ref->get_line() + 1;
			out.col = ref->get_column() + 1;
		}
		else
		{
			out.path = display_name;
		}
		return out;
	}

	std::string below_range_message(int16_t note)
	{
		return "Below the MDSDRV/ROM melodic range: lowest useful note is o1 c. Lower notes clamp to o1 c ("
				+ std::to_string(note)
				+ " < "
				+ std::to_string(kMdsdrvRomNoteMin)
				+ ")";
	}

	std::string above_range_message(int16_t note)
	{
		return "Above the MDSDRV/ROM melodic range: ROM export will fail ("
				+ std::to_string(note)
				+ " > "
				+ std::to_string(kMdsdrvRomNoteExclusiveMax)
				+ ")";
	}

	// Tracks referenced by drum mode notes are note lookup tables, not
	// playable channel data, so their note params are exempt.
	std::set<int16_t> collect_drum_subroutine_tracks(Song &song)
	{
		std::set<int16_t> drum_subroutine_tracks;
		for (auto &[track_id, track] : song.get_track_map())
		{
			(void)track_id;
			bool drum_mode_enabled = false;
			for (unsigned long i = 0; i < track.get_event_count(); ++i)
			{
				auto &event = track.get_event(i);
				if (event.type == Event::DRUM_MODE)
				{
					drum_mode_enabled = event.param != 0;
					continue;
				}
				if (event.type == Event::NOTE && drum_mode_enabled)
					drum_subroutine_tracks.insert(event.param);
			}
		}
		return drum_subroutine_tracks;
	}

	// Direct scan of the channel tracks themselves; macro tracks are covered
	// by MacroNoteRangeChecker below.
	void collect_direct_warnings(
			Song &song,
			const std::string &display_name,
			const std::set<int16_t> &drum_subroutine_tracks,
			std::vector<ctrmml_cmd::CheckMessage> &warnings)
	{
		for (auto &[track_id, track] : song.get_track_map())
		{
			if (track_id >= kMegadriveChannelTrackCount)
				continue;
			if (drum_subroutine_tracks.count(static_cast<int16_t>(track_id)))
				continue;
			bool in_drum_mode = false;
			for (unsigned long i = 0; i < track.get_event_count(); ++i)
			{
				auto &event = track.get_event(i);
				if (event.type == Event::DRUM_MODE)
				{
					in_drum_mode = event.param != 0;
					continue;
				}
				if (event.type != Event::NOTE || in_drum_mode)
					continue;

				if (event.param < kMdsdrvRomNoteMin)
				{
					warnings.push_back(make_message_from_ref(
							event.reference,
							display_name,
							below_range_message(event.param) + ".",
							"rom_note_range_warning"));
				}
				else if (event.param >= kMdsdrvRomNoteExclusiveMax)
				{
					warnings.push_back(make_message_from_ref(
							event.reference,
							display_name,
							above_range_message(event.param) + ".",
							"rom_note_range_warning"));
				}
			}
		}
	}

	// Walks a channel track through Event::JUMP into macro tracks, keeping
	// the invoking channel's drum mode context. Notes that only exist inside
	// macros are range-checked here; a note is skipped when the channel is in
	// drum mode at that point, because it selects a drum subroutine instead
	// of a melodic pitch.
	class MacroNoteRangeChecker : public Basic_Player
	{
	public:
		MacroNoteRangeChecker(
				Song &song,
				Track &track,
				uint16_t channel_id,
				const std::string &display_name,
				const std::unordered_map<const Event *, uint16_t> &event_track_ids,
				const std::set<int16_t> &drum_subroutine_tracks,
				std::set<const Event *> &reported_events,
				std::vector<ctrmml_cmd::CheckMessage> &warnings)
				: Basic_Player(song, track),
					channel_id(channel_id),
					display_name(display_name),
					event_track_ids(event_track_ids),
					drum_subroutine_tracks(drum_subroutine_tracks),
					reported_events(reported_events),
					warnings(warnings)
		{
			while (is_enabled())
				step_event();
		}

	private:
		void event_hook() override
		{
			if (!track_event)
				return;

			if (event.type == Event::DRUM_MODE)
			{
				drum_mode_enabled = event.param != 0;
				return;
			}
			if (event.type != Event::NOTE || drum_mode_enabled)
				return;
			if (event.param >= kMdsdrvRomNoteMin && event.param < kMdsdrvRomNoteExclusiveMax)
				return;

			auto track_it = event_track_ids.find(track_event);
			if (track_it == event_track_ids.end())
				return;
			uint16_t event_track_id = track_it->second;
			bool covered_by_direct_scan = event_track_id < kMegadriveChannelTrackCount
					&& !drum_subroutine_tracks.count(static_cast<int16_t>(event_track_id));
			if (covered_by_direct_scan || !reported_events.insert(track_event).second)
				return;

			std::string message = event.param < kMdsdrvRomNoteMin
					? below_range_message(event.param)
					: above_range_message(event.param);
			message += " (channel ";
			message += static_cast<char>('A' + channel_id);
			message += ", macro *" + std::to_string(event_track_id) + ")";
			warnings.push_back(make_message_from_ref(
					event.reference,
					display_name,
					message,
					"rom_note_range_warning"));
		}

		bool loop_hook() override
		{
			return false;
		}

		void end_hook() override
		{
		}

		uint16_t channel_id;
		std::string display_name;
		const std::unordered_map<const Event *, uint16_t> &event_track_ids;
		const std::set<int16_t> &drum_subroutine_tracks;
		std::set<const Event *> &reported_events;
		std::vector<ctrmml_cmd::CheckMessage> &warnings;
		bool drum_mode_enabled = false;
	};
}

namespace ctrmml_cmd
{
	std::vector<CheckMessage> collect_note_range_warnings(
			Song &song,
			const std::string &display_name)
	{
		std::vector<CheckMessage> warnings;
		auto drum_subroutine_tracks = collect_drum_subroutine_tracks(song);
		collect_direct_warnings(song, display_name, drum_subroutine_tracks, warnings);

		std::unordered_map<const Event *, uint16_t> event_track_ids;
		for (auto &[track_id, track] : song.get_track_map())
		{
			for (auto &track_event : track.get_events())
				event_track_ids.emplace(&track_event, track_id);
		}

		std::set<const Event *> reported_events;
		for (auto &[track_id, track] : song.get_track_map())
		{
			if (track_id >= kMegadriveChannelTrackCount)
				break;
			if (drum_subroutine_tracks.count(static_cast<int16_t>(track_id)))
				continue;
			MacroNoteRangeChecker checker(
					song,
					track,
					track_id,
					display_name,
					event_track_ids,
					drum_subroutine_tracks,
					reported_events,
					warnings);
		}
		return warnings;
	}
}
