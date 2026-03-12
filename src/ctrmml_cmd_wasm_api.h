#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	// Compilation
	int ctrmml_cmd_wasm_compile(const char *text, const char *base_dir);
	const char *ctrmml_cmd_wasm_get_compile_error();
	const char *ctrmml_cmd_wasm_check_json(const char *text, const char *base_dir);

	// Real-time playback
	int ctrmml_cmd_wasm_start_playback(uint32_t sample_rate,
																		 uint32_t start_line,
																		 uint32_t start_col);
	void ctrmml_cmd_wasm_stop_playback();
	int ctrmml_cmd_wasm_is_playing();
	int ctrmml_cmd_wasm_render_audio(float *output, int frames);

	// Tick tracking + highlights
	uint32_t ctrmml_cmd_wasm_get_player_ticks();
	uint32_t ctrmml_cmd_wasm_get_highlights(uint32_t ticks,
																					uint32_t *lines,
																					uint32_t *cols,
																					uint32_t max_entries);

	// Export (writes to MEMFS paths, JS reads via FS.readFile)
	int ctrmml_cmd_wasm_export_vgm(const char *text,
																 const char *base_dir,
																 const char *out_path);
	int ctrmml_cmd_wasm_export_wav(const char *text,
																 const char *base_dir,
																 const char *out_path);
	int ctrmml_cmd_wasm_quickrom(const char **input_paths,
															 int count,
															 const char *out_path);
	int ctrmml_cmd_wasm_mdslink(const char **input_paths,
															int count,
															const char *seq_out,
															const char *pcm_out);
	const char *ctrmml_cmd_wasm_get_last_error();

	// Track data (JSON) — visualization
	const char *ctrmml_cmd_wasm_get_track_data_json();

#ifdef __cplusplus
}
#endif
