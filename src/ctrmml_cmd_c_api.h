#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctrmml_cmd_string_result {
	int ok;
	char* message;
} ctrmml_cmd_string_result;

ctrmml_cmd_string_result ctrmml_cmd_check_file(const char* path);
ctrmml_cmd_string_result ctrmml_cmd_check_text(const char* text,
	const char* base_dir,
	const char* display_name);

void ctrmml_cmd_free_string_result(ctrmml_cmd_string_result result);

#ifdef __cplusplus
}
#endif
