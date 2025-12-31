#include "ctrmml_cmd_c_api.h"

#include <cstring>

#include "ctrmml_cmd.h"

namespace
{
char* dup_cstr(const std::string& value)
{
	char* out = static_cast<char*>(std::malloc(value.size() + 1));
	if (!out)
		return nullptr;
	std::memcpy(out, value.c_str(), value.size() + 1);
	return out;
}
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_check_file(const char* path)
{
	if (!path)
		return {0, dup_cstr("invalid path")};
	auto result = ctrmml_cmd::check_file(path);
	return {result.ok ? 1 : 0, dup_cstr(result.error)};
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_check_text(
	const char* text,
	const char* base_dir,
	const char* display_name)
{
	if (!text || !base_dir || !display_name)
		return {0, dup_cstr("invalid input")};
	auto result = ctrmml_cmd::check_text(text, base_dir, display_name);
	return {result.ok ? 1 : 0, dup_cstr(result.error)};
}

extern "C" void ctrmml_cmd_free_string_result(ctrmml_cmd_string_result result)
{
	std::free(result.message);
}
