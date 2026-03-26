#include "ctrmml_cmd_wasm_api.h"

#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ctrmml_cmd.h"
#include "highlight_tracker.h"
#include "mdslink_tool.h"
#include "mml_compile.h"
#include "rom_builder.h"
#include "template_rom_data.h"
#include "vgm_audio_renderer.h"
#include "vgm_export.h"
#include "wav_export.h"

#ifdef __EMSCRIPTEN__
// Stubs for libvgm charset conversion (not needed in WASM).
#include <utils/StrUtils.h>
extern "C"
{
	UINT8 CPConv_Init(CPCONV **, const char *, const char *) { return 0xFF; }
	void CPConv_Deinit(CPCONV *) {}
	UINT8 CPConv_StrConvert(CPCONV *, size_t *, char **, size_t, const char *) { return 0xFF; }
}
#endif

namespace
{
	const float kInvSampleScale = 1.0f / 8388608.0f;

	CompileResult g_compile;
	std::unique_ptr<VgmAudioRenderer> g_renderer;

	std::string g_last_error;
	std::string g_check_json_result;
	std::string g_track_json;

	void set_error(const std::string &msg)
	{
		g_last_error = msg;
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

	auto report = ctrmml_cmd::check_text_report(text, base_dir, "input.mml");
	g_check_json_result = ctrmml_cmd::check_report_json(report);
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

	if (!g_compile.song || !g_compile.tracks || !g_compile.lines)
	{
		set_error("no compiled song");
		return 1;
	}

	try
	{
		uint32_t start_ticks = 0;
		if (start_line > 0 || start_col > 0)
			start_ticks = find_start_ticks(*g_compile.song, *g_compile.lines, start_line, start_col);

		g_renderer = std::make_unique<VgmAudioRenderer>(g_compile.song, start_ticks, false);
		g_renderer->setup_stream(sample_rate);
	}
	catch (std::exception &e)
	{
		set_error(e.what());
		g_renderer.reset();
		return 1;
	}

	return 0;
}

extern "C" void ctrmml_cmd_wasm_set_mute_mask(int32_t chip_id, uint32_t mask)
{
	if (g_renderer)
		g_renderer->set_mute_mask(chip_id, mask);
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
	if (!g_renderer || !output || frames <= 0)
		return 0;

	std::vector<WAVE_32BS> scratch(frames);
	int written = g_renderer->get_sample(scratch.data(), frames);
	if (written < 0)
		written = 0;

	for (int i = 0; i < written; ++i)
	{
		float l = scratch[i].L * kInvSampleScale;
		float r = scratch[i].R * kInvSampleScale;
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
	if (!text || !base_dir || !out_path)
	{
		set_error("invalid input");
		return 1;
	}

	if (!export_vgm_text(text, base_dir, "input.mml", out_path))
	{
		set_error("VGM export failed");
		return 1;
	}
	return 0;
}

extern "C" int ctrmml_cmd_wasm_export_wav(const char *text,
																					const char *base_dir,
																					const char *out_path)
{
	if (!text || !base_dir || !out_path)
	{
		set_error("invalid input");
		return 1;
	}

	if (!export_wav_text(text, base_dir, "input.mml", out_path))
	{
		set_error("WAV export failed");
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
	if (!result.ok)
	{
		set_error(result.error);
		return 1;
	}
	return 0;
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

	auto result = run_mdslink(opts);
	if (!result.ok)
	{
		set_error(result.error);
		return 1;
	}
	return 0;
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
