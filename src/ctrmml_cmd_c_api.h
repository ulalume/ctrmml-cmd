#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctrmml_cmd_string_result {
	int ok;
	char* message;
} ctrmml_cmd_string_result;

typedef struct ctrmml_cmd_highlight_position {
	uint32_t line;
	uint32_t col;
} ctrmml_cmd_highlight_position;

typedef void (*ctrmml_cmd_highlight_callback)(uint32_t ticks,
	const ctrmml_cmd_highlight_position* positions,
	size_t count,
	void* user_data);

typedef struct ctrmml_cmd_stop_flag ctrmml_cmd_stop_flag;

typedef struct ctrmml_cmd_play_options {
	int follow;
	int log_messages;
	int has_start;
	uint32_t start_line;
	uint32_t start_col;
	ctrmml_cmd_stop_flag* stop_flag;
	ctrmml_cmd_highlight_callback on_highlight;
	void* user_data;
} ctrmml_cmd_play_options;

ctrmml_cmd_string_result ctrmml_cmd_check_file(const char* path);
ctrmml_cmd_string_result ctrmml_cmd_check_text(const char* text,
	const char* base_dir,
	const char* display_name);

ctrmml_cmd_string_result ctrmml_cmd_export_vgm_file(const char* path, const char* out_path);
ctrmml_cmd_string_result ctrmml_cmd_export_vgm_text(const char* text,
	const char* base_dir,
	const char* display_name,
	const char* out_path);
ctrmml_cmd_string_result ctrmml_cmd_export_wav_file(const char* path, const char* out_path);
ctrmml_cmd_string_result ctrmml_cmd_export_wav_text(const char* text,
	const char* base_dir,
	const char* display_name,
	const char* out_path);

ctrmml_cmd_stop_flag* ctrmml_cmd_stop_flag_new(void);
void ctrmml_cmd_stop_flag_set(ctrmml_cmd_stop_flag* flag);
void ctrmml_cmd_stop_flag_free(ctrmml_cmd_stop_flag* flag);

ctrmml_cmd_string_result ctrmml_cmd_play_file(const char* path,
	const ctrmml_cmd_play_options* options);
ctrmml_cmd_string_result ctrmml_cmd_play_text(const char* text,
	const char* base_dir,
	const char* display_name,
	const ctrmml_cmd_play_options* options);

void ctrmml_cmd_free_string_result(ctrmml_cmd_string_result result);

#ifdef __cplusplus
}
#endif
