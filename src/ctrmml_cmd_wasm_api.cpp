#include "ctrmml_cmd_wasm_api.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include "ctrmml_cmd.h"
#include "exception_text.h"
#include "highlight_tracker.h"
#include "input.h"
#include "mdslink_tool.h"
#include "mml_compile.h"
#include "platform/mdsdrv.h"
#include "rom_builder.h"
#include "song_blob.h"
#include "template_rom_data.h"
#include "vgm_export.h"
#include "wav_export.h"

namespace
{
	Ym2612ChipType g_ym2612_chip_type = Ym2612ChipType::Ym2612;

	CompileResult g_compile;
	std::unique_ptr<MDSDRV_Data> g_instrument_data;
	bool g_instrument_data_stale = false;

	std::string g_last_error;
	std::string g_check_json_result;
	std::string g_track_json;
	std::string g_channel_json;
	RomBuildResult g_quickrom_result;
	size_t g_quickrom_rom_size = 0;
	std::string g_quickrom_info;
	MdslinkPayload g_mdslink_payload;
	std::string g_mdslink_info;

	void set_error(const std::string &msg)
	{
		g_last_error = msg;
	}

	/** Instrument tables of g_compile.song for the JSON queries, read on the
	 *  first query after g_compile changes. Null if there is no song or it
	 *  cannot be read. */
	const MDSDRV_Data *instrument_data()
	{
		if (g_instrument_data_stale)
		{
			g_instrument_data_stale = false;
			g_instrument_data.reset();
			if (g_compile.song)
			{
				try
				{
					auto data = std::make_unique<MDSDRV_Data>();
					data->read_song(*g_compile.song);
					g_instrument_data = std::move(data);
				}
				catch (std::exception &)
				{
				}
			}
		}
		return g_instrument_data.get();
	}

	/** Populate MdslinkOptions.inputs from a char** array. Returns false on null entry. */
	bool populate_inputs(MdslinkOptions &opts, const char **input_paths, int count)
	{
		for (int i = 0; i < count; ++i)
		{
			if (!input_paths[i])
			{
				set_error("null input path");
				return false;
			}
			opts.inputs.push_back(input_paths[i]);
		}
		return true;
	}
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------

extern "C" int ctrmml_cmd_wasm_compile(const char *text, const char *base_dir)
{
	g_last_error.clear();

	if (!text || !base_dir)
	{
		set_error("invalid input");
		return 1;
	}

	g_compile = compile_mml_text(text, base_dir, "input.mml");
	g_instrument_data_stale = true;
	if (!g_compile.song)
	{
		set_error(g_compile.error);
		return 1;
	}
	return 0;
}

extern "C" const char *ctrmml_cmd_wasm_get_compile_error()
{
	return g_last_error.c_str();
}

extern "C" const char *ctrmml_cmd_wasm_check_json(const char *text,
																									 const char *base_dir)
{
	if (!text || !base_dir)
	{
		g_check_json_result = R"({"errors":[{"message":"invalid input"}],"warnings":[]})";
		return g_check_json_result.c_str();
	}

	auto report = ctrmml_cmd::check_text_report(text, base_dir, "input.mml", &g_compile);
	g_check_json_result = ctrmml_cmd::check_report_json(report);
	g_instrument_data_stale = true;

	return g_check_json_result.c_str();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

extern "C" uint8_t *ctrmml_cmd_wasm_pack_song(uint32_t *size)
{
	g_last_error.clear();
	if (size)
		*size = 0;
	if (!g_compile.song)
	{
		set_error("no compiled song");
		return nullptr;
	}
	try
	{
		const std::vector<uint8_t> packed = serialize_song(*g_compile.song, true);
		auto *bytes = static_cast<uint8_t *>(std::malloc(packed.size()));
		if (!bytes)
			throw std::bad_alloc();
		std::memcpy(bytes, packed.data(), packed.size());
		if (size)
			*size = static_cast<uint32_t>(packed.size());
		return bytes;
	}
	catch (...)
	{
		set_error(current_exception_text());
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// Highlights
// ---------------------------------------------------------------------------

extern "C" uint32_t ctrmml_cmd_wasm_get_highlights(uint32_t ticks,
																											uint32_t *lines,
																											uint32_t *cols,
																											uint32_t max_entries)
{
	if (!g_compile.tracks || !lines || !cols || max_entries == 0)
		return 0;

	auto positions = collect_highlights(*g_compile.tracks, ticks, max_entries);
	uint32_t count = static_cast<uint32_t>(positions.size());
	for (uint32_t i = 0; i < count; ++i)
	{
		lines[i] = positions[i].line;
		cols[i] = positions[i].col;
	}
	return count;
}

extern "C" int32_t ctrmml_cmd_wasm_find_cursor_tick(uint32_t line, uint32_t col)
{
	if (!g_compile.song || !g_compile.lines)
		return -1;
	return find_cursor_tick(*g_compile.song, *g_compile.lines, line, col);
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

extern "C" int ctrmml_cmd_wasm_export_vgm(const char *text,
																			const char *base_dir,
																			const char *out_path)
{
	g_last_error.clear();

	if (!text || !base_dir || !out_path)
	{
		set_error("invalid input");
		return 1;
	}

	auto result = export_vgm_text(text, base_dir, "input.mml", out_path);
	if (!result.ok)
	{
		set_error(result.error);
		return 1;
	}
	return 0;
}

extern "C" void ctrmml_cmd_wasm_set_ym2612_chip_type(int chip_type)
{
	g_ym2612_chip_type = chip_type == static_cast<int>(Ym2612ChipType::Ym3438)
			? Ym2612ChipType::Ym3438
			: Ym2612ChipType::Ym2612;
}

extern "C" int ctrmml_cmd_wasm_export_wav(const char *text,
																			const char *base_dir,
																			const char *out_path)
{
	g_last_error.clear();

	if (!text || !base_dir || !out_path)
	{
		set_error("invalid input");
		return 1;
	}

	auto result = export_wav_text(text, base_dir, "input.mml", out_path,
			g_ym2612_chip_type);
	if (!result.ok)
	{
		set_error(result.error);
		return 1;
	}
	return 0;
}

extern "C" int ctrmml_cmd_wasm_quickrom(const char **input_paths,
																				int count,
																				const char *out_path)
{
	if (!input_paths || count <= 0 || !out_path)
	{
		set_error("invalid input");
		return 1;
	}

	MdslinkOptions mds_opts;
	if (!populate_inputs(mds_opts, input_paths, count))
		return 1;

	RomBuildOptions rom_opts;
	rom_opts.mdslink = mds_opts;
	rom_opts.template_rom_bytes.assign(
			ctrmml_embedded::kTemplateRomData,
			ctrmml_embedded::kTemplateRomData + ctrmml_embedded::kTemplateRomSize);
	rom_opts.output_rom_path = out_path;

	auto result = run_rom_build(rom_opts);
	g_quickrom_result = result;
	g_quickrom_rom_size = 0;
	if (!result.ok)
	{
		set_error(result.error);
		return 1;
	}
	std::error_code ec;
	auto fsize = std::filesystem::file_size(out_path, ec);
	if (!ec)
		g_quickrom_rom_size = static_cast<size_t>(fsize);
	return 0;
}

extern "C" const char *ctrmml_cmd_wasm_get_quickrom_info()
{
	g_quickrom_info = format_rom_build_summary(g_quickrom_result, g_quickrom_rom_size);
	return g_quickrom_info.c_str();
}

extern "C" int ctrmml_cmd_wasm_mdslink(const char **input_paths,
																			 int count,
																			 const char *seq_out,
																			 const char *pcm_out,
																			 const char *c_header_out,
																			 const char *asm_header_out)
{
	if (!input_paths || count <= 0 || !seq_out || !pcm_out || !c_header_out || !asm_header_out)
	{
		set_error("invalid input");
		return 1;
	}

	MdslinkOptions opts;
	if (!populate_inputs(opts, input_paths, count))
		return 1;
	opts.seq_output = seq_out;
	opts.pcm_output = pcm_out;
	opts.c_header_output = c_header_out;
	opts.asm_header_output = asm_header_out;

	auto build = build_mdslink_payload(opts);
	if (!build.ok)
	{
		set_error(build.error);
		return 1;
	}
	auto written = write_mdslink_outputs(opts, build.payload);
	if (!written.ok)
	{
		set_error(written.error);
		return 1;
	}
	g_mdslink_payload = std::move(build.payload);
	return 0;
}

extern "C" const char *ctrmml_cmd_wasm_get_mdslink_info()
{
	g_mdslink_info = format_mdslink_build_summary(g_mdslink_payload);
	return g_mdslink_info.c_str();
}

extern "C" const char *ctrmml_cmd_wasm_get_last_error()
{
	return g_last_error.c_str();
}

// ---------------------------------------------------------------------------
// Track data (JSON) — for piano roll / visualization
// ---------------------------------------------------------------------------

extern "C" const char *ctrmml_cmd_wasm_get_track_data_json()
{
	if (!g_compile.song || !g_compile.tracks)
	{
		g_track_json = R"({"ppqn":0,"tracks":[]})";
		return g_track_json.c_str();
	}

	std::ostringstream os;
	os << "{\"ppqn\":" << g_compile.song->get_ppqn() << ",\"tracks\":[";

	bool first_track = true;
	for (auto &[track_id, info] : *g_compile.tracks)
	{
		if (!first_track) os << ',';
		first_track = false;

		os << "{\"id\":" << track_id
		   << ",\"length\":" << info.length
		   << ",\"loop_start\":" << info.loop_start
		   << ",\"loop_length\":" << info.loop_length
		   << ",\"events\":[";

		bool first_event = true;
		for (auto &[tick, ev] : info.events)
		{
			if (!first_event) os << ',';
			first_event = false;

			os << "{\"tick\":" << tick
			   << ",\"note\":" << ev.note
			   << ",\"on_time\":" << ev.on_time
			   << ",\"off_time\":" << ev.off_time
			   << ",\"instrument\":" << ev.instrument
			   << ",\"volume\":" << ev.volume
			   << ",\"transpose\":" << ev.transpose
			   << ",\"is_tie\":" << (ev.is_tie ? "true" : "false")
			   << ",\"is_slur\":" << (ev.is_slur ? "true" : "false")
			   << ",\"coarse_volume\":" << (ev.coarse_volume_flag ? "true" : "false")
			   << ",\"pitch_envelope\":" << ev.pitch_envelope
			   << ",\"portamento\":" << ev.portamento
			   << '}';
		}

		os << "]}";
	}

	os << "]}";
	g_track_json = os.str();
	return g_track_json.c_str();
}

// ---------------------------------------------------------------------------
// Cursor channel info (JSON) — instrument data at cursor position
// ---------------------------------------------------------------------------

namespace
{
	// Map YM2612 register address → FM data_bank byte index.
	// data_bank physical order: i=0:OP1, i=1:OP3, i=2:OP2, i=3:OP4
	// Register offsets:  +0=OP1, +4=OP3, +8=OP2, +C=OP4
	int fm_reg_to_data_index(uint8_t reg)
	{
		// TL uses special MDSDRV addresses: 0xfc=OP1, 0xfd=OP3, 0xfe=OP2,
		// 0xff=OP4 → data[24..27] in the same physical order
		if (reg >= 0xfc)
			return 24 + (reg - 0xfc);
		// FB/ALG: 0xB0 → data[28]
		if (reg == 0xb0)
			return 28;
		// DT/MUL 0x30..0x3C → data[0..3], then KS/AR 0x50, AM/DR 0x60,
		// SR 0x70, SL/RR 0x80 and SSG-EG 0x90 → data[4..23]
		const int row = reg >> 4;
		if ((row != 0x3 && (row < 0x5 || row > 0x9)) || (reg & 0x0f) > 0x0c)
			return -1;
		return (row == 0x3 ? 0 : (row - 0x4) * 4) + ((reg >> 2) & 3);
	}

	const char *instrument_type_name(MDSDRV_Data::InstrumentType type)
	{
		switch (type)
		{
		case MDSDRV_Data::INS_FM:  return "fm";
		case MDSDRV_Data::INS_PSG: return "psg";
		case MDSDRV_Data::INS_PCM: return "pcm";
		default: return "unknown";
		}
	}

	// Writes `,"data":[...]`.
	void write_data_array(std::ostringstream &os, const std::vector<uint8_t> &data)
	{
		os << ",\"data\":[";
		for (size_t i = 0; i < data.size(); ++i)
		{
			if (i > 0) os << ',';
			os << static_cast<int>(data[i]);
		}
		os << ']';
	}

	struct TrackEventRange
	{
		unsigned long before_line;  // events added by the lines before the cursor line
		unsigned long through_line; // the same plus those added by the cursor line
	};

	// CompileLineMap holds each track's event count after a line was read, for
	// the lines that added to that track.
	TrackEventRange track_events_around_line(const CompileLineMap &lines, int line, uint16_t track_id)
	{
		TrackEventRange range{0, 0};
		auto line_it = lines.lower_bound(line);
		for (auto prev = std::make_reverse_iterator(line_it); prev != lines.rend(); ++prev)
		{
			auto track_it = prev->second.find(track_id);
			if (track_it != prev->second.end())
			{
				range.before_line = track_it->second;
				break;
			}
		}
		range.through_line = range.before_line;
		if (line_it != lines.end() && line_it->first == line)
		{
			auto track_it = line_it->second.find(track_id);
			if (track_it != line_it->second.end())
				range.through_line = track_it->second;
		}
		return range;
	}
}

extern "C" const char *ctrmml_cmd_wasm_find_cursor_channel_json(uint32_t line, uint32_t col)
{
	const MDSDRV_Data *ins_data = instrument_data();
	if (!g_compile.song || !g_compile.lines || !ins_data)
	{
		g_channel_json = "null";
		return g_channel_json.c_str();
	}

	// Find which track(s) have content on this line
	auto line_it = g_compile.lines->find(static_cast<int>(line));
	if (line_it == g_compile.lines->end())
	{
		g_channel_json = "null";
		return g_channel_json.c_str();
	}

	const auto &line_map = line_it->second;
	int best_track_id = -1;

	// For chord-split syntax like `ABCD {c/f/a+/>d+}`, multiple tracks share
	// the same source line but each NOTE event has its own column. Pick the
	// track whose latest event on this line has the largest column not past
	// the cursor. line_map[line][track] is the event count AFTER parsing
	// this line, so iterate [0, end) and filter by reference line == cursor.
	int best_event_col = -1;
	for (const auto &[track_id, end_pos] : line_map)
	{
		if (track_id >= kChannelTrackCount)
			continue;
		try
		{
			Track &track = g_compile.song->get_track(track_id);
			for (unsigned long i = 0; i < end_pos; ++i)
			{
				Event &event = track.get_event(i);
				auto ref = event.reference;
				if (!ref)
					continue;
				if (ref->get_line() != static_cast<int>(line))
					continue;
				int ev_col = ref->get_column();
				if (ev_col > static_cast<int>(col))
					continue;
				if (ev_col > best_event_col)
				{
					best_event_col = ev_col;
					best_track_id = static_cast<int>(track_id);
				}
			}
		}
		catch (std::exception &)
		{
		}
	}

	// Fall back to the first track on the line when no event matched
	// (e.g. cursor is before any chord slot or in leading whitespace).
	if (best_track_id < 0)
	{
		for (const auto &[track_id, end_pos] : line_map)
		{
			if (track_id >= kChannelTrackCount)
				continue;
			best_track_id = track_id;
			break;
		}
	}

	if (best_track_id < 0)
	{
		g_channel_json = "null";
		return g_channel_json.c_str();
	}

	const auto range = track_events_around_line(*g_compile.lines, static_cast<int>(line),
			static_cast<uint16_t>(best_track_id));
	auto is_before_cursor = [&](const Event &event) {
		const auto &ref = event.reference;
		return ref && (ref->get_line() < static_cast<int>(line) ||
				(ref->get_line() == static_cast<int>(line) && ref->get_column() < static_cast<int>(col)));
	};

	// Scan raw Track events to find:
	// - active instrument (last Event::INS) and last note (Event::NOTE), from
	//   earlier lines and from the cursor line before the cursor column
	// - FM register overrides (Event::PLATFORM) and fm3 flags, likewise
	const auto &ins_type_map = ins_data->get_ins_type_map();
	const auto &env_map = ins_data->get_envelope_map_view();
	const auto &data_bank = ins_data->get_data_bank();

	uint16_t ins_id = 0;
	int fm3_flags = -1;
	int last_note = -1;
	std::vector<uint8_t> effective_data;

	struct FmOverride { uint8_t reg; int value; bool relative; };
	std::vector<FmOverride> pending_overrides;

	try
	{
		Track &track = g_compile.song->get_track(best_track_id);

		for (unsigned long i = 0; i < range.through_line; ++i)
		{
			Event &event = track.get_event(i);
			bool before_cursor_line = (i < range.before_line);

			if (event.type == Event::NOTE)
			{
				if (before_cursor_line || is_before_cursor(event))
					last_note = event.param;
			}
			else if (event.type == Event::INS)
			{
				if (before_cursor_line || is_before_cursor(event))
				{
					ins_id = static_cast<uint16_t>(event.param);
					pending_overrides.clear();
					fm3_flags = -1;
				}
			}
			else if (event.type == Event::PLATFORM && (before_cursor_line || is_before_cursor(event)))
			{
				try
				{
					Tag &cmd = g_compile.song->get_platform_command(event.param);
					if (cmd.empty()) continue;
					const std::string &cmd_name = cmd[0];

					if (cmd_name == "fm3" && cmd.size() >= 2)
					{
						fm3_flags = static_cast<int>(
							0x80 | ((std::strtol(cmd[1].c_str(), nullptr, 2) ^ 0x0f) & 0x0f));
						continue;
					}

					if (cmd.size() < 2) continue;
					uint8_t reg = MDSDRV_get_register(cmd_name);
					if (reg == 0) continue;

					int value = std::strtol(cmd[1].c_str(), nullptr, 10);
					bool relative = (reg >= 0xfc) && (cmd[1][0] == '+' || cmd[1][0] == '-');
					pending_overrides.push_back({reg, value, relative});
				}
				catch (std::exception &)
				{
					continue;
				}
			}
		}
	}
	catch (std::exception &)
	{
		// Track access failed
	}

	// Look up instrument type
	auto type_it = ins_type_map.find(ins_id);
	if (type_it == ins_type_map.end())
	{
		g_channel_json = "null";
		return g_channel_json.c_str();
	}

	// Copy base instrument data and apply overrides
	auto env_it = env_map.find(ins_id);
	if (env_it != env_map.end() &&
		env_it->second >= 0 &&
		static_cast<size_t>(env_it->second) < data_bank.size())
	{
		effective_data = data_bank[env_it->second];
	}

	if (type_it->second == MDSDRV_Data::INS_FM && !effective_data.empty())
	{
		for (const auto &ov : pending_overrides)
		{
			int data_idx = fm_reg_to_data_index(ov.reg);
			if (data_idx < 0 || static_cast<size_t>(data_idx) >= effective_data.size())
				continue;
			if (ov.relative)
			{
				int cur = effective_data[data_idx];
				effective_data[data_idx] = static_cast<uint8_t>(
					std::max(0, std::min(127, cur + ov.value)));
			}
			else
			{
				effective_data[data_idx] = static_cast<uint8_t>(ov.value);
			}
		}
	}

	std::ostringstream os;
	os << "{\"track_id\":" << best_track_id
	   << ",\"instrument_id\":" << ins_id
	   << ",\"instrument_type\":\"" << instrument_type_name(type_it->second) << "\"";

	if (!effective_data.empty())
		write_data_array(os, effective_data);

	if (fm3_flags >= 0)
		os << ",\"fm3_flags\":" << fm3_flags;

	// Octave derived from last note event (note / 12). Default = MML o6 (DEFAULT_OCTAVE=5).
	int octave = (last_note >= 0) ? (last_note / 12) : 5;
	os << ",\"octave\":" << octave;

	os << '}';
	g_channel_json = os.str();
	return g_channel_json.c_str();
}

// ---------------------------------------------------------------------------
// Instrument data lookup by ID
// ---------------------------------------------------------------------------

namespace { std::string g_ins_json; }

extern "C" const char *ctrmml_cmd_wasm_get_instrument_data_json(uint16_t ins_id)
{
	const MDSDRV_Data *ins_data = instrument_data();
	if (!ins_data)
	{
		g_ins_json = "null";
		return g_ins_json.c_str();
	}

	const auto &type_map = ins_data->get_ins_type_map();
	auto type_it = type_map.find(ins_id);
	if (type_it == type_map.end())
	{
		g_ins_json = "null";
		return g_ins_json.c_str();
	}

	const auto &env_map = ins_data->get_envelope_map_view();
	const auto &data_bank = ins_data->get_data_bank();
	auto env_it = env_map.find(ins_id);

	std::ostringstream os;
	os << "{\"instrument_type\":\"" << instrument_type_name(type_it->second) << "\"";

	if (env_it != env_map.end() && env_it->second >= 0 &&
		static_cast<size_t>(env_it->second) < data_bank.size())
		write_data_array(os, data_bank[env_it->second]);

	os << '}';
	g_ins_json = os.str();
	return g_ins_json.c_str();
}
