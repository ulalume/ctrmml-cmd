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
	void ctrmml_cmd_wasm_set_mute_mask(int32_t chip_id, uint32_t mask);

	// Tick tracking + highlights
	uint32_t ctrmml_cmd_wasm_get_player_ticks();
	uint32_t ctrmml_cmd_wasm_get_highlights(uint32_t ticks,
																					uint32_t *lines,
																					uint32_t *cols,
																					uint32_t max_entries);
	int32_t ctrmml_cmd_wasm_find_cursor_tick(uint32_t line, uint32_t col);

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
															const char *pcm_out,
															const char *c_header_out,
															const char *asm_header_out);
	const char *ctrmml_cmd_wasm_get_last_error();

	// Track data (JSON) — visualization
	const char *ctrmml_cmd_wasm_get_track_data_json();

	// Cursor channel info (JSON) — instrument data at cursor position
	const char *ctrmml_cmd_wasm_find_cursor_channel_json(uint32_t line, uint32_t col);

	// Preview synth — standalone chip emulator for note preview / MIDI
	void preview_init(uint32_t sample_rate);
	void preview_deinit();
	void preview_load_fm(const uint8_t *data, int len);
	void preview_load_psg(const uint8_t *data, int len);
	void preview_set_mode(int mode);
	void preview_note_on(uint8_t midi_note, uint8_t velocity);
	void preview_note_off(uint8_t midi_note);
	void preview_all_notes_off();

#ifdef __cplusplus
}
#endif
