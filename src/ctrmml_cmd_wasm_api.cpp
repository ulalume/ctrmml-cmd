#include "ctrmml_cmd_wasm_api.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ctrmml_cmd.h"
#include "dc_blocker.h"
#include "highlight_tracker.h"
#include "input.h"
#include "lowpass_filter.h"
#include "mdslink_tool.h"
#include "mml_compile.h"
#include "platform/mdsdrv.h"
#include "preview_synth.h"
#include "rom_builder.h"
#include "template_rom_data.h"
#include "vgm_audio_renderer.h"
#include "vgm_export.h"
#include "wav_export.h"

namespace
{
	const float kInvSampleScale = 1.0f / 8388608.0f;
	constexpr int kMaxChannels = 16;
	LowPassFilter g_lpf;
	DcBlocker g_dc_blocker;
	Ym2612ChipType g_ym2612_chip_type = Ym2612ChipType::Ym2612;

	CompileResult g_compile;
	std::unique_ptr<VgmAudioRenderer> g_renderer;
	std::unique_ptr<MDSDRV_Data> g_instrument_data;
	std::unique_ptr<PreviewSynth> g_preview;

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

	/** Build g_renderer at `start_ticks` with `sample_rate`. On failure sets
	 *  g_last_error, clears g_renderer, and returns false. */
	bool build_renderer(uint32_t start_ticks, uint32_t sample_rate)
	{
		try
		{
			g_renderer = std::make_unique<VgmAudioRenderer>(g_compile.song, start_ticks, false);
			g_renderer->set_ym2612_chip_type(g_ym2612_chip_type);
			g_renderer->setup_stream(sample_rate);
			g_lpf.init(sample_rate);
			g_dc_blocker.reset();
			return true;
		}
		catch (std::exception &e)
		{
			set_error(e.what());
			g_renderer.reset();
			return false;
		}
	}

	void rebuild_instrument_data()
	{
		if (!g_compile.song)
		{
			g_instrument_data.reset();
			return;
		}
		try
		{
			g_instrument_data = std::make_unique<MDSDRV_Data>();
			g_instrument_data->read_song(*g_compile.song);
		}
		catch (std::exception &)
		{
			g_instrument_data.reset();
		}
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

	void json_escape(std::ostringstream &os, const char *s)
	{
		os << '"';
		for (; *s; ++s)
		{
			switch (*s)
			{
			case '"': os << "\\\""; break;
			case '\\': os << "\\\\"; break;
			case '\n': os << "\\n"; break;
			case '\r': os << "\\r"; break;
			case '\t': os << "\\t"; break;
			default: os << *s;
			}
		}
		os << '"';
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
	if (!g_compile.song)
	{
		set_error(g_compile.error);
		g_instrument_data.reset();
		return 1;
	}

	rebuild_instrument_data();
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
	rebuild_instrument_data();

	return g_check_json_result.c_str();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

extern "C" int ctrmml_cmd_wasm_start_playback(uint32_t sample_rate,
															uint32_t start_line,
															uint32_t start_col)
{
	g_renderer.reset();
	g_last_error.clear();

	if (!g_compile.song || !g_compile.tracks || !g_compile.lines)
	{
		set_error("no compiled song");
		return 1;
	}

	uint32_t start_ticks = 0;
	try
	{
		if (start_line > 0 || start_col > 0)
			start_ticks = find_start_ticks(*g_compile.song, *g_compile.lines, start_line, start_col);
	}
	catch (std::exception &e)
	{
		set_error(e.what());
		return 1;
	}

	return build_renderer(start_ticks, sample_rate) ? 0 : 1;
}

extern "C" int ctrmml_cmd_wasm_compile_and_relink(const char *text, const char *base_dir)
{
	g_last_error.clear();

	if (!text || !base_dir)
	{
		set_error("invalid input");
		return 1;
	}

	uint32_t current_tick = 0;
	bool had_renderer = false;
	if (g_renderer && !g_renderer->is_finished())
	{
		auto &driver = g_renderer->get_driver();
		if (driver)
		{
			current_tick = driver->get_player_ticks();
			had_renderer = true;
		}
	}

	// On compile error leave existing state untouched so the renderer keeps
	// playing the previously-compiled song.
	CompileResult new_compile = compile_mml_text(text, base_dir, "input.mml");
	if (!new_compile.song)
	{
		set_error(new_compile.error);
		return 1;
	}

	g_compile = std::move(new_compile);
	rebuild_instrument_data();

	if (had_renderer)
	{
		try
		{
			g_renderer->relink_song(g_compile.song, current_tick);
		}
		catch (std::exception &e)
		{
			set_error(e.what());
			g_renderer.reset();
			return 1;
		}
	}

	return 0;
}

extern "C" void ctrmml_cmd_wasm_set_mute_mask(int32_t chip_id, uint32_t mask)
{
	if (g_renderer)
		g_renderer->set_mute_mask(chip_id, mask);
}

extern "C" void ctrmml_cmd_wasm_set_ym2612_chip_type(int chip_type)
{
	g_ym2612_chip_type = chip_type == static_cast<int>(Ym2612ChipType::Ym3438)
			? Ym2612ChipType::Ym3438
			: Ym2612ChipType::Ym2612;
	if (g_renderer)
		g_renderer->set_ym2612_chip_type(g_ym2612_chip_type);
	if (g_preview)
		g_preview->set_ym2612_chip_type(g_ym2612_chip_type);
}

extern "C" void ctrmml_cmd_wasm_stop_playback()
{
	if (g_renderer)
		g_renderer->stop_playback();
	g_renderer.reset();
}

extern "C" int ctrmml_cmd_wasm_is_playing()
{
	if (!g_renderer)
		return 0;
	return g_renderer->is_finished() ? 0 : 1;
}

extern "C" int ctrmml_cmd_wasm_render_audio(float *output, int frames)
{
	if (!output || frames <= 0)
		return 0;

	bool has_main = g_renderer && !g_renderer->is_finished();
	bool has_preview = g_preview && g_preview->is_active();

	if (!has_main && !has_preview)
		return 0;

	static std::vector<WAVE_32BS> scratch;
	if (static_cast<int>(scratch.size()) < frames)
		scratch.resize(frames);
	std::memset(scratch.data(), 0, frames * sizeof(WAVE_32BS));
	int written = 0;

	if (has_main)
	{
		written = g_renderer->get_sample(scratch.data(), frames);
		if (written < 0)
			written = 0;
		if (g_renderer->is_finished() && !g_renderer->last_error().empty())
			set_error(g_renderer->last_error());
	}

	if (has_preview)
	{
		if (written == 0)
		{
			std::memset(scratch.data(), 0, frames * sizeof(WAVE_32BS));
			written = frames;
		}
		g_preview->render(scratch.data(), written);
	}

	for (int i = 0; i < written; ++i)
	{
		g_lpf.apply(scratch[i].L, scratch[i].R);
		float l = scratch[i].L * kInvSampleScale;
		float r = scratch[i].R * kInvSampleScale;
		g_dc_blocker.process(l, r);
		if (l > 1.0f) l = 1.0f;
		if (l < -1.0f) l = -1.0f;
		if (r > 1.0f) r = 1.0f;
		if (r < -1.0f) r = -1.0f;
		output[i * 2 + 0] = l;
		output[i * 2 + 1] = r;
	}

	return written;
}

// ---------------------------------------------------------------------------
// Tick tracking + highlights
// ---------------------------------------------------------------------------

extern "C" uint32_t ctrmml_cmd_wasm_get_player_ticks()
{
	if (!g_renderer)
		return 0;
	auto &driver = g_renderer->get_driver();
	return driver ? driver->get_player_ticks() : 0;
}

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
		// Map register low nibble to data_bank slot within a group
		// 0x*0→0(OP1), 0x*4→1(OP3), 0x*8→2(OP2), 0x*C→3(OP4)
		static const std::map<uint8_t, int> reg_to_slot = {
			{0x00, 0}, {0x04, 1}, {0x08, 2}, {0x0c, 3}
		};

		auto slot_it = reg_to_slot.find(reg & 0x0c);

		// DT/MUL: 0x30..0x3C → data[0..3]
		if (reg >= 0x30 && reg <= 0x3c && slot_it != reg_to_slot.end())
			return 0 + slot_it->second;
		// KS/AR: 0x50..0x5C → data[4..7]
		if (reg >= 0x50 && reg <= 0x5c && slot_it != reg_to_slot.end())
			return 4 + slot_it->second;
		// AM/DR: 0x60..0x6C → data[8..11]
		if (reg >= 0x60 && reg <= 0x6c && slot_it != reg_to_slot.end())
			return 8 + slot_it->second;
		// SR: 0x70..0x7C → data[12..15]
		if (reg >= 0x70 && reg <= 0x7c && slot_it != reg_to_slot.end())
			return 12 + slot_it->second;
		// SL/RR: 0x80..0x8C → data[16..19]
		if (reg >= 0x80 && reg <= 0x8c && slot_it != reg_to_slot.end())
			return 16 + slot_it->second;
		// SSG-EG: 0x90..0x9C → data[20..23]
		if (reg >= 0x90 && reg <= 0x9c && slot_it != reg_to_slot.end())
			return 20 + slot_it->second;

		// TL uses special MDSDRV addresses: 0xfc=OP1, 0xfd=OP3, 0xfe=OP2, 0xff=OP4
		// → data[24..27] in same physical order
		static const std::map<uint8_t, int> tl_map = {
			{0xfc, 24}, {0xfd, 25}, {0xfe, 26}, {0xff, 27}
		};
		auto tl_it = tl_map.find(reg);
		if (tl_it != tl_map.end()) return tl_it->second;

		// FB/ALG: 0xB0 → data[28]
		if (reg == 0xb0) return 28;

		return -1;
	}
}

extern "C" const char *ctrmml_cmd_wasm_find_cursor_channel_json(uint32_t line, uint32_t col)
{
	if (!g_compile.song || !g_compile.lines || !g_compile.tracks || !g_instrument_data)
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
		if (track_id >= kMaxChannels)
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
			if (track_id >= kMaxChannels)
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

	// Determine the event scan boundary from CompileLineMap.
	// line_map[track_id] = event count at the START of parsing that line,
	// i.e. events with index < this value were added by earlier lines.
	// Use the cursor line's own entry as the boundary so that events
	// added ON the cursor line are excluded (they come after the cursor).
	unsigned long scan_limit = 0;
	{
		auto bound_it = g_compile.lines->lower_bound(static_cast<int>(line));
		bool found = false;
		while (bound_it != g_compile.lines->end())
		{
			auto tk = bound_it->second.find(static_cast<uint16_t>(best_track_id));
			if (tk != bound_it->second.end())
			{
				scan_limit = tk->second;
				found = true;
				break;
			}
			++bound_it;
		}
		if (!found)
		{
			// No later line; scan all events in the track
			try
			{
				scan_limit = g_compile.song->get_track(best_track_id).get_event_count();
			}
			catch (std::exception &)
			{
				scan_limit = 0;
			}
		}
	}

	// Scan raw Track events to find:
	// - active instrument (last Event::INS)
	// - FM register overrides (Event::PLATFORM) — only BEFORE cursor line
	// - fm3 flags — only BEFORE cursor line
	// - last note (Event::NOTE) — including cursor line, up to cursor column
	const auto &ins_type_map = g_instrument_data->get_ins_type_map();
	const auto &env_map = g_instrument_data->get_envelope_map_view();
	const auto &data_bank = g_instrument_data->get_data_bank();

	uint16_t ins_id = 0;
	int fm3_flags = -1;
	int last_note = -1;
	std::vector<uint8_t> effective_data;

	struct FmOverride { uint8_t reg; int value; bool relative; };
	std::vector<FmOverride> pending_overrides;

	// Determine extended scan limit (includes cursor line) for NOTE/INS
	unsigned long extended_limit = 0;
	{
		auto upper_it = g_compile.lines->upper_bound(static_cast<int>(line));
		bool found = false;
		while (upper_it != g_compile.lines->end())
		{
			auto tk = upper_it->second.find(static_cast<uint16_t>(best_track_id));
			if (tk != upper_it->second.end())
			{
				extended_limit = tk->second;
				found = true;
				break;
			}
			++upper_it;
		}
		if (!found)
		{
			try { extended_limit = g_compile.song->get_track(best_track_id).get_event_count(); }
			catch (std::exception &) { extended_limit = 0; }
		}
	}

	try
	{
		Track &track = g_compile.song->get_track(best_track_id);

		for (unsigned long i = 0; i < extended_limit; ++i)
		{
			Event &event = track.get_event(i);
			bool before_cursor_line = (i < scan_limit);

			if (event.type == Event::NOTE)
			{
				// Accept notes from cursor line only if their source is before cursor col
				if (before_cursor_line)
				{
					last_note = event.param;
				}
				else
				{
					auto ref = event.reference;
					if (ref && (ref->get_line() < static_cast<int>(line) ||
								(ref->get_line() == static_cast<int>(line) &&
								 ref->get_column() < static_cast<int>(col))))
					{
						last_note = event.param;
					}
				}
			}
			else if (event.type == Event::INS)
			{
				ins_id = static_cast<uint16_t>(event.param);
				if (before_cursor_line)
				{
					pending_overrides.clear();
					fm3_flags = -1;
				}
			}
			else if (event.type == Event::PLATFORM && before_cursor_line)
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

	const char *type_str = "unknown";
	switch (type_it->second)
	{
	case MDSDRV_Data::INS_FM:  type_str = "fm";  break;
	case MDSDRV_Data::INS_PSG: type_str = "psg"; break;
	case MDSDRV_Data::INS_PCM: type_str = "pcm"; break;
	default: break;
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
	   << ",\"instrument_type\":\"" << type_str << "\"";

	if (!effective_data.empty())
	{
		os << ",\"data\":[";
		for (size_t i = 0; i < effective_data.size(); ++i)
		{
			if (i > 0) os << ',';
			os << static_cast<int>(effective_data[i]);
		}
		os << ']';
	}

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
	if (!g_instrument_data)
	{
		g_ins_json = "null";
		return g_ins_json.c_str();
	}

	const auto &type_map = g_instrument_data->get_ins_type_map();
	auto type_it = type_map.find(ins_id);
	if (type_it == type_map.end())
	{
		g_ins_json = "null";
		return g_ins_json.c_str();
	}

	const char *type_str = "unknown";
	switch (type_it->second)
	{
	case MDSDRV_Data::INS_FM:  type_str = "fm";  break;
	case MDSDRV_Data::INS_PSG: type_str = "psg"; break;
	case MDSDRV_Data::INS_PCM: type_str = "pcm"; break;
	default: break;
	}

	const auto &env_map = g_instrument_data->get_envelope_map_view();
	const auto &data_bank = g_instrument_data->get_data_bank();
	auto env_it = env_map.find(ins_id);

	std::ostringstream os;
	os << "{\"instrument_type\":\"" << type_str << "\"";

	if (env_it != env_map.end() && env_it->second >= 0 &&
		static_cast<size_t>(env_it->second) < data_bank.size())
	{
		const auto &data = data_bank[env_it->second];
		os << ",\"data\":[";
		for (size_t i = 0; i < data.size(); ++i)
		{
			if (i > 0) os << ',';
			os << static_cast<int>(data[i]);
		}
		os << ']';
	}

	os << '}';
	g_ins_json = os.str();
	return g_ins_json.c_str();
}

// ---------------------------------------------------------------------------
// Audio low-pass filter
// ---------------------------------------------------------------------------

extern "C" void ctrmml_cmd_wasm_set_lowpass_filter(int enabled, float cutoff_hz)
{
	g_lpf.enabled = (enabled != 0);
	if (g_lpf.enabled && cutoff_hz > 0.0f)
		g_lpf.set_cutoff(static_cast<double>(cutoff_hz));
}

// Preview synth
// ---------------------------------------------------------------------------

extern "C" void preview_init(uint32_t sample_rate)
{
	g_preview = std::make_unique<PreviewSynth>();
	g_preview->set_ym2612_chip_type(g_ym2612_chip_type);
	g_preview->init(sample_rate);
	g_lpf.init(sample_rate);
	g_dc_blocker.reset();
}

extern "C" void preview_deinit()
{
	g_preview.reset();
}

extern "C" void preview_load_fm(const uint8_t *data, int len)
{
	if (g_preview)
		g_preview->load_fm(data, len);
}

extern "C" void preview_load_psg(const uint8_t *data, int len)
{
	if (g_preview)
		g_preview->load_psg(data, len);
}

extern "C" void preview_set_mode(int mode)
{
	if (g_preview)
		g_preview->set_mode(mode);
}

extern "C" void preview_note_on(uint8_t midi_note, uint8_t velocity)
{
	if (g_preview)
		g_preview->note_on(midi_note, velocity);
}

extern "C" void preview_note_off(uint8_t midi_note)
{
	if (g_preview)
		g_preview->note_off(midi_note);
}

extern "C" void preview_set_fm_op_mask(uint8_t mask)
{
	if (g_preview)
		g_preview->set_fm_op_mask(mask);
}

extern "C" void preview_all_notes_off()
{
	if (g_preview)
		g_preview->all_notes_off();
}
