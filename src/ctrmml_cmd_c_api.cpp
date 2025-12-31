#include "ctrmml_cmd_c_api.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <vector>

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

struct StopFlag
{
	std::atomic<bool> value{false};
};

ctrmml_cmd::PlayOptions build_play_options(const ctrmml_cmd_play_options* options)
{
	ctrmml_cmd::PlayOptions out;
	if (!options)
		return out;

	out.follow = options->follow != 0;
	out.log_messages = options->log_messages != 0;
	out.has_start = options->has_start != 0;
	out.start_line = options->start_line;
	out.start_col = options->start_col;
	if (options->stop_flag)
		out.stop_flag = &reinterpret_cast<StopFlag*>(options->stop_flag)->value;

	if (options->on_highlight)
	{
		ctrmml_cmd_highlight_callback cb = options->on_highlight;
		void* user_data = options->user_data;
		out.on_highlight = [cb, user_data](uint32_t ticks,
			const std::vector<ctrmml_cmd::HighlightPosition>& positions) {
			std::vector<ctrmml_cmd_highlight_position> converted;
			converted.reserve(positions.size());
			for (const auto& pos : positions)
				converted.push_back({pos.line, pos.col});
			cb(ticks, converted.data(), converted.size(), user_data);
		};
	}

	return out;
}

ctrmml_cmd_string_result to_string_result(const ctrmml_cmd::CheckResult& result)
{
	return {result.ok ? 1 : 0, dup_cstr(result.error)};
}

ctrmml_cmd_string_result to_string_result(const ctrmml_cmd::ExportResult& result)
{
	return {result.ok ? 1 : 0, dup_cstr(result.error)};
}

ctrmml_cmd_string_result to_string_result(const ctrmml_cmd::PlayResult& result)
{
	return {result.ok ? 1 : 0, dup_cstr(result.error)};
}
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_check_file(const char* path)
{
	if (!path)
		return {0, dup_cstr("invalid path")};
	auto result = ctrmml_cmd::check_file(path);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_check_text(
	const char* text,
	const char* base_dir,
	const char* display_name)
{
	if (!text || !base_dir || !display_name)
		return {0, dup_cstr("invalid input")};
	auto result = ctrmml_cmd::check_text(text, base_dir, display_name);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_export_vgm_file(const char* path, const char* out_path)
{
	if (!path || !out_path)
		return {0, dup_cstr("invalid input")};
	auto result = ctrmml_cmd::export_vgm_file(path, out_path);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_export_vgm_text(
	const char* text,
	const char* base_dir,
	const char* display_name,
	const char* out_path)
{
	if (!text || !base_dir || !display_name || !out_path)
		return {0, dup_cstr("invalid input")};
	auto result = ctrmml_cmd::export_vgm_text(text, base_dir, display_name, out_path);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_export_wav_file(const char* path, const char* out_path)
{
	if (!path || !out_path)
		return {0, dup_cstr("invalid input")};
	auto result = ctrmml_cmd::export_wav_file(path, out_path);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_export_wav_text(
	const char* text,
	const char* base_dir,
	const char* display_name,
	const char* out_path)
{
	if (!text || !base_dir || !display_name || !out_path)
		return {0, dup_cstr("invalid input")};
	auto result = ctrmml_cmd::export_wav_text(text, base_dir, display_name, out_path);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_stop_flag* ctrmml_cmd_stop_flag_new(void)
{
	return reinterpret_cast<ctrmml_cmd_stop_flag*>(new StopFlag());
}

extern "C" void ctrmml_cmd_stop_flag_set(ctrmml_cmd_stop_flag* flag)
{
	if (!flag)
		return;
	reinterpret_cast<StopFlag*>(flag)->value.store(true);
}

extern "C" void ctrmml_cmd_stop_flag_free(ctrmml_cmd_stop_flag* flag)
{
	delete reinterpret_cast<StopFlag*>(flag);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_play_file(
	const char* path,
	const ctrmml_cmd_play_options* options)
{
	if (!path)
		return {0, dup_cstr("invalid path")};
	auto play_options = build_play_options(options);
	auto result = ctrmml_cmd::play_file(path, play_options);
	return to_string_result(result);
}

extern "C" ctrmml_cmd_string_result ctrmml_cmd_play_text(
	const char* text,
	const char* base_dir,
	const char* display_name,
	const ctrmml_cmd_play_options* options)
{
	if (!text || !base_dir || !display_name)
		return {0, dup_cstr("invalid input")};
	auto play_options = build_play_options(options);
	auto result = ctrmml_cmd::play_text(text, base_dir, display_name, play_options);
	return to_string_result(result);
}

extern "C" void ctrmml_cmd_free_string_result(ctrmml_cmd_string_result result)
{
	std::free(result.message);
}
