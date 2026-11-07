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

	// The compiled song as the player module loads it: serialize_song() with
	// source locations. The bytes come from malloc(); free() them once copied.
	// Null on failure, with the reason in ctrmml_cmd_wasm_get_last_error().
	uint8_t *ctrmml_cmd_wasm_pack_song(uint32_t *size);

	// Highlights
	uint32_t ctrmml_cmd_wasm_get_highlights(uint32_t ticks,
																					uint32_t *lines,
																					uint32_t *cols,
																					uint32_t max_entries);
	int32_t ctrmml_cmd_wasm_find_cursor_tick(uint32_t line, uint32_t col);

	// Export (writes to MEMFS paths, JS reads via FS.readFile)
	// The chip of WAV exports: 0 = YM2612 ladder DAC (default), 1 = clean YM3438 DAC.
	void ctrmml_cmd_wasm_set_ym2612_chip_type(int chip_type);
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
	const char *ctrmml_cmd_wasm_get_quickrom_info();
	const char *ctrmml_cmd_wasm_get_mdslink_info();

	// Track data (JSON) — visualization
	const char *ctrmml_cmd_wasm_get_track_data_json();

	// Cursor channel info (JSON) — instrument data at cursor position
	const char *ctrmml_cmd_wasm_find_cursor_channel_json(uint32_t line, uint32_t col);

	// Instrument data lookup by ID
	const char *ctrmml_cmd_wasm_get_instrument_data_json(uint16_t ins_id);

#ifdef __cplusplus
}
#endif
