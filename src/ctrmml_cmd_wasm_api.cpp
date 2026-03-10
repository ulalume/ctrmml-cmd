#include "ctrmml_cmd_wasm_api.h"

#include <cstring>
#include <memory>
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

	void set_error(const std::string &msg)
	{
		g_last_error = msg;
	}
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------

extern "C" int ctrmml_cmd_wasm_compile(const char *text, const char *base_dir)
{
	g_renderer.reset();
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

	if (!g_compile.song || !g_compile.tracks)
	{
		set_error("no compiled song");
		return 1;
	}

	try
	{
		uint32_t start_ticks = 0;
		if (start_line > 0 || start_col > 0)
			start_ticks = find_start_ticks(*g_compile.tracks, start_line, start_col);

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

extern "C" int ctrmml_cmd_wasm_quickrom(const char *input_path,
																				const char *out_path)
{
	if (!input_path || !out_path)
	{
		set_error("invalid input");
		return 1;
	}

	MdslinkOptions mds_opts;
	mds_opts.inputs.push_back(input_path);

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

extern "C" int ctrmml_cmd_wasm_mdslink(const char *input_path,
																			 const char *seq_out,
																			 const char *pcm_out)
{
	if (!input_path || !seq_out || !pcm_out)
	{
		set_error("invalid input");
		return 1;
	}

	MdslinkOptions opts;
	opts.inputs.push_back(input_path);
	opts.seq_output = seq_out;
	opts.pcm_output = pcm_out;

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
