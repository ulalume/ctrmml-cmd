#include "ctrmml_cmd.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>

#include "audio_output.h"
#include "highlight_tracker.h"
#include "mml_compile.h"
#include "vgm_audio_renderer.h"
#include "vgm_export.h"
#include "wav_export.h"
#include "input.h"

namespace
{
std::string preflight_playback_error(const std::shared_ptr<Song>& song)
{
	try
	{
		VgmAudioRenderer renderer(song, 0, false);
		renderer.setup_stream(44100);
		WAVE_32BS sample[1] = {};
		renderer.get_sample(sample, 1);
		if (!renderer.last_error().empty())
			return std::string("Playback error: ") + renderer.last_error();
	}
	catch (InputError& e)
	{
		return std::string("Playback error: ") + e.what();
	}
	catch (std::exception& e)
	{
		return std::string("Playback error: ") + e.what();
	}
	return std::string();
}

struct MissingSample
{
	uint32_t line;
	uint32_t col;
	std::string path;
};

static bool is_word_char(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

static std::optional<MissingSample> find_missing_pcm_sample(
	const std::string& text,
	const std::filesystem::path& base_dir)
{
	std::istringstream stream(text);
	std::string line;
	uint32_t line_number = 1;
	while (std::getline(stream, line))
	{
		std::string_view view(line);
		auto comment_pos = view.find(';');
		if (comment_pos != std::string_view::npos)
			view = view.substr(0, comment_pos);

		size_t search_pos = 0;
		while (true)
		{
			size_t pos = view.find("pcm", search_pos);
			if (pos == std::string_view::npos)
				break;
			bool start_ok = (pos == 0) || !is_word_char(view[pos - 1]);
			bool end_ok = (pos + 3 >= view.size()) || !is_word_char(view[pos + 3]);
			search_pos = pos + 3;
			if (!start_ok || !end_ok)
				continue;
			size_t quote_start = view.find('"', pos + 3);
			if (quote_start == std::string_view::npos)
				continue;
			size_t quote_end = view.find('"', quote_start + 1);
			if (quote_end == std::string_view::npos)
				continue;
			std::string sample_path = std::string(view.substr(quote_start + 1, quote_end - quote_start - 1));
			if (sample_path.empty())
				continue;
			std::filesystem::path fs_path(sample_path);
			if (!fs_path.is_absolute())
				fs_path = base_dir / fs_path;
			if (!std::filesystem::exists(fs_path))
			{
				return MissingSample{
					line_number,
					static_cast<uint32_t>(quote_start + 2),
					sample_path,
				};
			}
		}
		line_number++;
	}
	return std::nullopt;
}

static std::string missing_pcm_error_line(
	const std::string& text,
	const std::filesystem::path& base_dir,
	const std::string& display_name)
{
	auto missing = find_missing_pcm_sample(text, base_dir);
	if (!missing)
		return std::string();
	std::ostringstream out;
	out << display_name << ':' << missing->line << ':' << missing->col
		<< ": missing pcm sample: " << missing->path;
	return out.str();
}

static ctrmml_cmd::CheckResult check_compile(
	const CompileResult& compile,
	const std::string& input,
	const std::filesystem::path& base_dir,
	const std::string& display_name)
{
	if (!compile.song)
	{
		return {false, compile.error};
	}

	auto playback_error = preflight_playback_error(compile.song);
	if (!playback_error.empty())
	{
		auto missing = missing_pcm_error_line(input, base_dir, display_name);
		if (!missing.empty())
			return {false, missing};
		return {false, playback_error};
	}

	return {true, std::string()};
}

static ctrmml_cmd::PlayResult play_compiled(
	const CompileResult& compile,
	const ctrmml_cmd::PlayOptions& options)
{
	if (!compile.song)
		return {false, compile.error};

	uint32_t start_ticks = 0;
	if (options.has_start && compile.tracks)
		start_ticks = find_start_ticks(*compile.tracks, options.start_line, options.start_col);

	VgmAudioRenderer renderer(compile.song, start_ticks, options.log_messages);
	renderer.setup_stream(44100);

	AudioOutput output;
	if (!output.start(&renderer, 44100))
		return {false, "audio output failed"};

	uint32_t last_ticks = 0xffffffffu;
	std::atomic<bool> local_stop(false);
	std::atomic<bool>* stop_flag = options.stop_flag ? options.stop_flag : &local_stop;
	while (!renderer.is_finished() && !stop_flag->load())
	{
		if (options.follow && compile.tracks && options.on_highlight)
		{
			auto driver = renderer.get_driver();
			uint32_t ticks = driver ? driver->get_player_ticks() : 0;
			if (ticks != last_ticks)
			{
				last_ticks = ticks;
				auto positions = collect_highlights(*compile.tracks, ticks, 64);
				std::vector<ctrmml_cmd::HighlightPosition> converted;
				converted.reserve(positions.size());
				for (const auto& pos : positions)
					converted.push_back({pos.line, pos.col});
				options.on_highlight(ticks, converted);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	renderer.stop_playback();
	output.stop();
	return {true, std::string()};
}
}

namespace ctrmml_cmd
{
CheckResult check_file(const std::string& path)
{
	auto compile = compile_mml_file(path);
	std::filesystem::path file_path = std::filesystem::absolute(path);
	std::ifstream in(path);
	std::ostringstream buffer;
	buffer << in.rdbuf();
	std::string input = buffer.str();
	return check_compile(compile, input, file_path.parent_path(), file_path.string());
}

CheckResult check_text(const std::string& text,
	const std::string& base_dir,
	const std::string& display_name)
{
	auto compile = compile_mml_text(text, base_dir, display_name);
	return check_compile(compile, text, base_dir, display_name);
}

ExportResult export_vgm_file(const std::string& path, const std::string& out_path)
{
	if (!export_vgm(path, out_path))
		return {false, "failed to export vgm data"};
	return {true, std::string()};
}

ExportResult export_vgm_text(const std::string& text,
	const std::string& base_dir,
	const std::string& display_name,
	const std::string& out_path)
{
	if (!::export_vgm_text(text, base_dir, display_name, out_path))
		return {false, "failed to export vgm data"};
	return {true, std::string()};
}

ExportResult export_wav_file(const std::string& path, const std::string& out_path)
{
	if (!export_wav(path, out_path))
		return {false, "failed to export wav data"};
	return {true, std::string()};
}

ExportResult export_wav_text(const std::string& text,
	const std::string& base_dir,
	const std::string& display_name,
	const std::string& out_path)
{
	if (!::export_wav_text(text, base_dir, display_name, out_path))
		return {false, "failed to export wav data"};
	return {true, std::string()};
}

PlayResult play_file(const std::string& path, const PlayOptions& options)
{
	auto compile = compile_mml_file(path);
	return play_compiled(compile, options);
}

PlayResult play_text(const std::string& text,
	const std::string& base_dir,
	const std::string& display_name,
	const PlayOptions& options)
{
	auto compile = compile_mml_text(text, base_dir, display_name);
	return play_compiled(compile, options);
}
}
