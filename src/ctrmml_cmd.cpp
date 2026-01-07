#include "ctrmml_cmd.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "input.h"
#include "mml_compile.h"
#include "vgm_audio_renderer.h"

namespace
{
	class StderrCapture
	{
	public:
		StderrCapture()
				: old_buf(std::cerr.rdbuf(buffer.rdbuf()))
		{
		}

		~StderrCapture()
		{
			std::cerr.rdbuf(old_buf);
		}

		std::string str() const
		{
			return buffer.str();
		}

	private:
		std::ostringstream buffer;
		std::streambuf *old_buf;
	};

	class StdoutSilencer
	{
	public:
		StdoutSilencer()
		{
			std::fflush(stdout);
#if defined(_WIN32)
			old_fd = _dup(_fileno(stdout));
			FILE *null_file = nullptr;
			if (freopen_s(&null_file, "NUL", "w", stdout) == 0)
				quiet = null_file;
#else
			old_fd = dup(fileno(stdout));
			FILE *null_file = std::fopen("/dev/null", "w");
			if (null_file)
			{
				dup2(fileno(null_file), fileno(stdout));
				quiet = null_file;
			}
#endif
		}

		~StdoutSilencer()
		{
			std::fflush(stdout);
#if defined(_WIN32)
			if (old_fd != -1)
			{
				_dup2(old_fd, _fileno(stdout));
				_close(old_fd);
			}
#else
			if (old_fd != -1)
			{
				dup2(old_fd, fileno(stdout));
				close(old_fd);
			}
#endif
			if (quiet)
				std::fclose(quiet);
		}

	private:
		int old_fd = -1;
		FILE *quiet = nullptr;
	};

	std::string preflight_playback_error(const std::shared_ptr<Song> &song)
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
		catch (InputError &e)
		{
			return std::string("Playback error: ") + e.what();
		}
		catch (std::exception &e)
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

	bool is_word_char(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	}

	std::vector<MissingSample> find_missing_pcm_samples(
			const std::string &text,
			const std::filesystem::path &base_dir)
	{
		std::vector<MissingSample> out;
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
				{
					search_pos = quote_end + 1;
					continue;
				}
				std::filesystem::path fs_path(sample_path);
				if (!fs_path.is_absolute())
					fs_path = base_dir / fs_path;
				if (!std::filesystem::exists(fs_path))
				{
					out.push_back(MissingSample{
							line_number,
							static_cast<uint32_t>(quote_start + 2),
							sample_path,
					});
				}
				search_pos = quote_end + 1;
			}
			line_number++;
		}
		return out;
	}

	bool parse_location_line(const std::string &line,
													 std::string &path,
													 uint32_t &line_no,
													 uint32_t &col,
													 std::string &message)
	{
		auto message_pos = line.find(": ");
		if (message_pos == std::string::npos)
			return false;
		std::string location = line.substr(0, message_pos);
		std::string message_text = line.substr(message_pos + 2);

		auto last_colon = location.rfind(':');
		if (last_colon == std::string::npos)
			return false;
		auto prev_colon = location.rfind(':', last_colon - 1);
		if (prev_colon == std::string::npos)
			return false;
		std::string line_text = location.substr(prev_colon + 1, last_colon - prev_colon - 1);
		std::string col_text = location.substr(last_colon + 1);
		try
		{
			line_no = static_cast<uint32_t>(std::stoul(line_text));
			col = static_cast<uint32_t>(std::stoul(col_text));
		}
		catch (...)
		{
			return false;
		}
		path = location.substr(0, prev_colon);
		message = message_text;
		return true;
	}

	ctrmml_cmd::CheckMessage make_message_from_line(
			const std::string &line,
			const std::string &code)
	{
		ctrmml_cmd::CheckMessage msg{};
		msg.code = code;
		if (!parse_location_line(line, msg.path, msg.line, msg.col, msg.message))
			msg.message = line;
		return msg;
	}

	ctrmml_cmd::CheckMessage make_message_from_raw(
			const std::string &raw,
			const std::string &code,
			bool strip_playback_prefix)
	{
		std::string first_line = raw;
		auto newline_pos = first_line.find('\n');
		if (newline_pos != std::string::npos)
			first_line = first_line.substr(0, newline_pos);
		if (strip_playback_prefix)
		{
			const char *prefix = "Playback error: ";
			constexpr size_t prefix_len = 16;
			if (first_line.rfind(prefix, 0) == 0)
				first_line = first_line.substr(prefix_len);
		}
		auto msg = make_message_from_line(first_line, code);
		msg.raw = raw;
		return msg;
	}

	std::vector<ctrmml_cmd::CheckMessage> parse_warnings(const std::string &captured)
	{
		std::vector<ctrmml_cmd::CheckMessage> warnings;
		std::istringstream stream(captured);
		std::string line;
		bool skip_next = false;
		while (std::getline(stream, line))
		{
			if (skip_next)
			{
				skip_next = false;
				continue;
			}
			if (line.empty())
				continue;
			auto warning = make_message_from_line(line, "parse_warning");
			if (!warning.message.empty())
				warnings.push_back(warning);
			if (!warning.path.empty())
				skip_next = true;
		}
		return warnings;
	}

	std::string format_message_line(const ctrmml_cmd::CheckMessage &msg)
	{
		if (!msg.raw.empty())
			return msg.raw;
		if (!msg.path.empty() && msg.line && msg.col)
		{
			std::ostringstream out;
			out << msg.path << ':' << msg.line << ':' << msg.col << ": " << msg.message;
			return out.str();
		}
		return msg.message;
	}

	ctrmml_cmd::CheckMessage make_missing_pcm_message(
			const std::string &display_name,
			const MissingSample &sample)
	{
		ctrmml_cmd::CheckMessage msg{};
		msg.code = "pcm_missing";
		msg.path = display_name;
		msg.line = sample.line;
		msg.col = sample.col;
		msg.message = std::string("missing pcm sample: ") + sample.path;
		msg.raw = format_message_line(msg);
		return msg;
	}

	template <typename CompileFn>
	ctrmml_cmd::CheckReport build_check_report(
			const std::string &text,
			const std::filesystem::path &base_dir,
			const std::string &display_name,
			CompileFn compile_fn)
	{
		ctrmml_cmd::CheckReport report{};
		auto missing = find_missing_pcm_samples(text, base_dir);
		for (const auto &sample : missing)
			report.errors.push_back(make_missing_pcm_message(display_name, sample));

		StdoutSilencer stdout_silencer;
		CompileResult compile{};
		std::vector<ctrmml_cmd::CheckMessage> warnings;
		{
			StderrCapture capture;
			compile = compile_fn();
			warnings = parse_warnings(capture.str());
		}
		report.warnings.insert(report.warnings.end(), warnings.begin(), warnings.end());

		if (!compile.song)
		{
			if (!compile.error.empty())
				report.errors.push_back(make_message_from_raw(compile.error, "parse_error", false));
			return report;
		}

		if (report.errors.empty())
		{
			auto playback_error = preflight_playback_error(compile.song);
			if (!playback_error.empty())
				report.errors.push_back(make_message_from_raw(playback_error, "playback_error", true));
		}

		return report;
	}

	void append_json_string(std::string &out, const std::string &value)
	{
		out.push_back('"');
		for (unsigned char c : value)
		{
			switch (c)
			{
			case '\\':
				out += "\\\\";
				break;
			case '"':
				out += "\\\"";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (c < 0x20)
				{
					char buf[7];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				}
				else
				{
					out.push_back(static_cast<char>(c));
				}
				break;
			}
		}
		out.push_back('"');
	}

	void append_json_messages(std::string &out, const std::vector<ctrmml_cmd::CheckMessage> &messages)
	{
		out.push_back('[');
		for (size_t i = 0; i < messages.size(); ++i)
		{
			if (i)
				out.push_back(',');
			out.push_back('{');
			out += "\"message\":";
			append_json_string(out, messages[i].message);
			out += ",\"path\":";
			append_json_string(out, messages[i].path);
			out += ",\"line\":";
			out += std::to_string(messages[i].line);
			out += ",\"col\":";
			out += std::to_string(messages[i].col);
			out += ",\"code\":";
			append_json_string(out, messages[i].code);
			out.push_back('}');
		}
		out.push_back(']');
	}

	ctrmml_cmd::CheckResult to_check_result(const ctrmml_cmd::CheckReport &report)
	{
		if (report.ok())
			return {true, std::string()};
		return {false, format_message_line(report.errors.front())};
	}
}

namespace ctrmml_cmd
{
	CheckReport check_text_report(const std::string &text,
																const std::string &base_dir,
																const std::string &display_name)
	{
		return build_check_report(
				text,
				base_dir,
				display_name,
				[&]() { return compile_mml_text(text, base_dir, display_name); });
	}

	CheckReport check_file_report(const std::string &path)
	{
		std::filesystem::path file_path = std::filesystem::absolute(path);
		std::ifstream in(path);
		std::ostringstream buffer;
		if (in)
			buffer << in.rdbuf();
		std::string input = buffer.str();
		return build_check_report(
				input,
				file_path.parent_path(),
				file_path.string(),
				[&]() { return compile_mml_file(path); });
	}

	std::string check_report_json(const CheckReport &report)
	{
		std::string out;
		out += "{\"ok\":";
		out += report.ok() ? "true" : "false";
		out += ",\"errors\":";
		append_json_messages(out, report.errors);
		out += ",\"warnings\":";
		append_json_messages(out, report.warnings);
		out += '}';
		return out;
	}

	CheckResult check_file(const std::string &path)
	{
		return to_check_result(check_file_report(path));
	}

	CheckResult check_text(const std::string &text,
												 const std::string &base_dir,
												 const std::string &display_name)
	{
		return to_check_result(check_text_report(text, base_dir, display_name));
	}
}
