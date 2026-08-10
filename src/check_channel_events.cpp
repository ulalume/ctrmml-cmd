#include "check_channel_events.h"

#include <cctype>
#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "input.h"
#include "player.h"
#include "song.h"
#include "track.h"

namespace
{
	constexpr uint16_t kFirstPsgTrack = 6;
	constexpr uint16_t kLastPsgTrack = 9;
	constexpr uint16_t kFirstDummyTrack = 10;
	constexpr uint16_t kChannelTrackCount = 16;

	enum class ForbiddenEventKind
	{
		NONE,
		PAN,
		LFO,
	};

	bool equals_ascii_case_insensitive(const std::string &value, const char *expected)
	{
		size_t i = 0;
		for (; i < value.size() && expected[i]; ++i)
		{
			if (std::tolower(static_cast<unsigned char>(value[i])) !=
					std::tolower(static_cast<unsigned char>(expected[i])))
				return false;
		}
		return i == value.size() && expected[i] == '\0';
	}

	ctrmml_cmd::CheckMessage make_warning(
			const std::shared_ptr<InputRef> &ref,
			const std::string &display_name,
			uint16_t channel_id,
			uint16_t event_track_id,
			bool via_jump,
			ForbiddenEventKind kind)
	{
		std::ostringstream message;
		if (kind == ForbiddenEventKind::PAN)
		{
			message << "Panning is not supported for PSG channels: in-app playback will stop here "
						"(MDSDRV hardware ignores it)";
		}
		else
		{
			message << "LFO is not supported for PSG channels: in-app playback will stop here "
						"(MDSDRV hardware interprets it as a noise-mode command)";
		}
		message << " (channel " << static_cast<char>('A' + channel_id);
		if (via_jump)
			message << ", macro *" << event_track_id;
		message << ')';

		ctrmml_cmd::CheckMessage out{};
		out.message = message.str();
		out.code = "playback_unsupported_warning";
		out.length = 1;
		if (ref)
		{
			out.path = ref->get_filename().empty() ? display_name : ref->get_filename();
			out.line = ref->get_line() + 1;
			out.col = ref->get_column() + 1;
		}
		else
		{
			out.path = display_name;
		}
		return out;
	}

	class ChannelEventChecker : public Basic_Player
	{
	public:
		ChannelEventChecker(
				Song &song,
				Track &track,
				uint16_t channel_id,
				const std::string &display_name)
				: Basic_Player(song, track),
					channel_id(channel_id),
					display_name(display_name)
		{
			for (auto &[track_id, candidate] : song.get_track_map())
			{
				for (auto &candidate_event : candidate.get_events())
					event_track_ids.emplace(&candidate_event, track_id);
			}

			while (is_enabled())
				step_event();
		}

		std::vector<ctrmml_cmd::CheckMessage> take_warnings()
		{
			return std::move(warnings);
		}

	private:
		void event_hook() override
		{
			if (!track_event)
				return;

			ForbiddenEventKind kind = forbidden_event_kind(event);
			if (kind == ForbiddenEventKind::NONE || !seen_events.insert(track_event).second)
				return;

			auto track_it = event_track_ids.find(track_event);
			uint16_t event_track_id = track_it == event_track_ids.end()
												? channel_id
												: track_it->second;
			warnings.push_back(make_warning(
					event.reference,
					display_name,
					channel_id,
					event_track_id,
					is_inside_jump(),
					kind));
		}

		bool loop_hook() override
		{
			return false;
		}

		void end_hook() override
		{
		}

		ForbiddenEventKind forbidden_event_kind(const Event &candidate)
		{
			if (channel_id < kFirstPsgTrack || channel_id > kLastPsgTrack)
				return ForbiddenEventKind::NONE;
			if (candidate.type == Event::PAN)
				return ForbiddenEventKind::PAN;
			if (candidate.type == Event::PAN_ENVELOPE)
				return candidate.param != 0
						? ForbiddenEventKind::PAN
						: ForbiddenEventKind::NONE;
			if (candidate.type != Event::PLATFORM)
				return ForbiddenEventKind::NONE;

			try
			{
				const Tag &tag = get_song()->get_platform_command(candidate.param);
				return !tag.empty() && equals_ascii_case_insensitive(tag.front(), "lfo")
						? ForbiddenEventKind::LFO
						: ForbiddenEventKind::NONE;
			}
			catch (const std::out_of_range &)
			{
				return ForbiddenEventKind::NONE;
			}
		}

		uint16_t channel_id;
		std::string display_name;
		std::unordered_map<const Event *, uint16_t> event_track_ids;
		std::set<const Event *> seen_events;
		std::vector<ctrmml_cmd::CheckMessage> warnings;
	};
}

namespace ctrmml_cmd
{
	std::vector<CheckMessage> collect_channel_event_warnings(
			Song &song,
			const std::string &display_name)
	{
		std::vector<CheckMessage> warnings;
		for (auto &[track_id, track] : song.get_track_map())
		{
			if (track_id >= kChannelTrackCount)
				break;
			if (track_id >= kFirstDummyTrack)
				continue;

			ChannelEventChecker checker(song, track, track_id, display_name);
			auto channel_warnings = checker.take_warnings();
			warnings.insert(
					warnings.end(),
					std::make_move_iterator(channel_warnings.begin()),
					std::make_move_iterator(channel_warnings.end()));
		}
		return warnings;
	}
}
