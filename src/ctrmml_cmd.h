#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ctrmml_cmd
{
struct HighlightPosition
{
	uint32_t line;
	uint32_t col;
};

struct CheckResult
{
	bool ok;
	std::string error;
};

struct ExportResult
{
	bool ok;
	std::string error;
};

struct PlayOptions
{
	bool follow = false;
	bool log_messages = true;
	bool has_start = false;
	uint32_t start_line = 0;
	uint32_t start_col = 0;
	std::atomic<bool>* stop_flag = nullptr;
	std::function<void(uint32_t, const std::vector<HighlightPosition>&)> on_highlight;
};

struct PlayResult
{
	bool ok;
	std::string error;
};

CheckResult check_file(const std::string& path);
CheckResult check_text(const std::string& text,
	const std::string& base_dir,
	const std::string& display_name);

ExportResult export_vgm_file(const std::string& path, const std::string& out_path);
ExportResult export_vgm_text(const std::string& text,
	const std::string& base_dir,
	const std::string& display_name,
	const std::string& out_path);
ExportResult export_wav_file(const std::string& path, const std::string& out_path);
ExportResult export_wav_text(const std::string& text,
	const std::string& base_dir,
	const std::string& display_name,
	const std::string& out_path);

PlayResult play_file(const std::string& path, const PlayOptions& options);
PlayResult play_text(const std::string& text,
	const std::string& base_dir,
	const std::string& display_name,
	const PlayOptions& options);
}
