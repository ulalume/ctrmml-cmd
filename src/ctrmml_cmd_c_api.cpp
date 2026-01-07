#include "ctrmml_cmd_c_api.h"

#include <cstdlib>
#include <cstring>

#include "ctrmml_cmd.h"

namespace
{
	char *dup_cstr(const std::string &value)
	{
		char *out = static_cast<char *>(std::malloc(value.size() + 1));
		if (!out)
			return nullptr;
		std::memcpy(out, value.c_str(), value.size() + 1);
		return out;
	}

	ctrmml_cmd_string_result to_string_result(const ctrmml_cmd::CheckResult &result)
	{
		return {result.ok ? 1 : 0, dup_cstr(result.error)};
	}
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_check_file(const char *path)
{
	if (!path)
		return {0, dup_cstr("invalid path")};
	auto result = ctrmml_cmd::check_file(path);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_check_text(
		const char *text,
		const char *base_dir,
		const char *display_name)
{
	if (!text || !base_dir || !display_name)
		return {0, dup_cstr("invalid input")};
	auto result = ctrmml_cmd::check_text(text, base_dir, display_name);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_check_file_json(const char *path)
{
	if (!path)
		return {0, dup_cstr("invalid path")};
	auto report = ctrmml_cmd::check_file_report(path);
	auto json = ctrmml_cmd::check_report_json(report);
	return {report.ok() ? 1 : 0, dup_cstr(json)};
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_check_text_json(
		const char *text,
		const char *base_dir,
		const char *display_name)
{
	if (!text || !base_dir || !display_name)
		return {0, dup_cstr("invalid input")};
	auto report = ctrmml_cmd::check_text_report(text, base_dir, display_name);
	auto json = ctrmml_cmd::check_report_json(report);
	return {report.ok() ? 1 : 0, dup_cstr(json)};
}

extern "C" void ctrmml_cmd_free_string_result(ctrmml_cmd_string_result result)
{
	std::free(result.message);
}
