#include "ctrmml_cmd.h"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string_view>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "input.h"
#include "check_supplemental.h"
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

	std::vector<ctrmml_cmd::CheckMessage> parse_warnings(
			const std::string &captured,
			const std::string &fallback_path)
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
			std::string path;
			uint32_t line_no = 0;
			uint32_t col = 0;
			std::string message;
			if (parse_location_line(line, path, line_no, col, message))
			{
				ctrmml_cmd::CheckMessage warning{};
				warning.code = "parse_warning";
				warning.message = message;
				warning.path = path.empty() ? fallback_path : path;
				warning.line = line_no;
				// operator<< outputs 0-indexed column, but the parser has already
				// advanced past the character, so the value is effectively 1-indexed.
				warning.col = col;
				warnings.push_back(warning);
				skip_next = true;
				continue;
			}
			auto warning = make_message_from_line(line, "parse_warning");
			if (!warning.message.empty())
				warnings.push_back(warning);
		}
		return warnings;
	}

	constexpr int kMdsdrvRomNoteMin = 0;
	constexpr int kMdsdrvRomNoteExclusiveMax = 94; // SLR - NOTE = 0xe0 - 0x82
	constexpr int kMegadriveChannelTrackCount = 16;

	ctrmml_cmd::CheckMessage make_message_from_ref(
			const std::shared_ptr<InputRef> &ref,
			const std::string &display_name,
			const std::string &message,
			const std::string &code,
			uint32_t length = 1)
	{
		ctrmml_cmd::CheckMessage out{};
		out.message = message;
		out.code = code;
		out.length = length;
		if (ref)
		{
			out.path = ref->get_filename().empty() ? display_name : ref->get_filename();
			// InputRef stores 0-indexed line and 0-indexed column; JSON consumers expect 1-indexed
			out.line = ref->get_line() + 1;
			out.col = ref->get_column() + 1;
		}
		else
		{
			out.path = display_name;
		}
		return out;
	}

	std::vector<ctrmml_cmd::CheckMessage> collect_rom_note_range_warnings(
			const std::shared_ptr<Song> &song,
			const std::string &display_name)
	{
		std::vector<ctrmml_cmd::CheckMessage> warnings;
		std::set<int16_t> drum_subroutine_tracks;

		for (auto &[track_id, track] : song->get_track_map())
		{
			(void)track_id;
			bool drum_mode_enabled = false;
			for (unsigned long i = 0; i < track.get_event_count(); ++i)
			{
				auto &event = track.get_event(i);
				if (event.type == Event::DRUM_MODE)
				{
					drum_mode_enabled = event.param != 0;
					continue;
				}
				if (event.type == Event::NOTE && drum_mode_enabled)
					drum_subroutine_tracks.insert(event.param);
			}
		}

		for (auto &[track_id, track] : song->get_track_map())
		{
			if (track_id >= kMegadriveChannelTrackCount)
				continue;
			if (drum_subroutine_tracks.count(static_cast<int16_t>(track_id)))
				continue;
			(void)track_id;
			bool in_drum_mode = false;
			for (unsigned long i = 0; i < track.get_event_count(); ++i)
			{
				auto &event = track.get_event(i);
				if (event.type == Event::DRUM_MODE)
				{
					in_drum_mode = event.param != 0;
					continue;
				}
				if (event.type != Event::NOTE || in_drum_mode)
					continue;

				if (event.param < kMdsdrvRomNoteMin)
				{
					warnings.push_back(make_message_from_ref(
							event.reference,
							display_name,
							"Below the MDSDRV/ROM melodic range: lowest useful note is o1 c. Lower notes clamp to o1 c ("
									+ std::to_string(event.param)
									+ " < "
									+ std::to_string(kMdsdrvRomNoteMin)
									+ ").",
							"rom_note_range_warning"));
				}
				else if (event.param >= kMdsdrvRomNoteExclusiveMax)
				{
					warnings.push_back(make_message_from_ref(
							event.reference,
							display_name,
							"Above the MDSDRV/ROM melodic range: ROM export will fail ("
									+ std::to_string(event.param)
									+ " > "
									+ std::to_string(kMdsdrvRomNoteExclusiveMax)
									+ ").",
							"rom_note_range_warning"));
				}
			}
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

	template <typename CompileFn>
	ctrmml_cmd::CheckReport build_check_report(
			const std::string &text,
			const std::filesystem::path &base_dir,
			const std::string &display_name,
			CompileFn compile_fn,
			CompileResult *out_compile = nullptr)
	{
		ctrmml_cmd::CheckReport report{};

		StdoutSilencer stdout_silencer;
		CompileResult compile{};
		std::vector<ctrmml_cmd::CheckMessage> warnings;
		{
			StderrCapture capture;
			compile = compile_fn();
			warnings = parse_warnings(capture.str(), display_name);
		}
		report.warnings.insert(report.warnings.end(), warnings.begin(), warnings.end());

		if (!compile.song)
		{
			if (!compile.error.empty())
			{
				auto message = make_message_from_raw(compile.error, "parse_error", false);
				if (message.path.empty() || message.line == 0 || message.col == 0)
				{
					auto mapped = ctrmml_cmd::SupplementalChecker(text, base_dir)
																.try_map_locationless_error(display_name, message.message, message.code);
					if (mapped.has_value())
					{
						mapped->raw = format_message_line(*mapped);
						report.errors.push_back(*mapped);
					}
					else
					{
						report.errors.push_back(message);
					}
				}
				else
				{
					report.errors.push_back(message);
				}
			}
			return report;
		}

		ctrmml_cmd::SupplementalChecker checker(text, base_dir);
		auto supplemental_errors = checker.collect_errors(display_name);
		report.errors.insert(report.errors.end(), supplemental_errors.begin(), supplemental_errors.end());
		auto range_warnings = collect_rom_note_range_warnings(compile.song, display_name);
		report.warnings.insert(report.warnings.end(), range_warnings.begin(), range_warnings.end());

		if (report.errors.empty())
		{
			auto playback_error = preflight_playback_error(compile.song);
			if (!playback_error.empty())
			{
				auto message = make_message_from_raw(playback_error, "playback_error", true);
				if (message.path.empty() || message.line == 0 || message.col == 0)
				{
					auto mapped = checker.try_map_locationless_error(display_name, message.message, message.code);
					if (mapped.has_value())
					{
						mapped->raw = format_message_line(*mapped);
						report.errors.push_back(*mapped);
					}
					else
					{
						report.errors.push_back(message);
					}
				}
				else
				{
					report.errors.push_back(message);
				}
			}
		}

		if (out_compile)
			*out_compile = std::move(compile);
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
			out += ",\"length\":";
			out += std::to_string(messages[i].length);
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
																const std::string &display_name,
																CompileResult *out_compile)
	{
		return build_check_report(
				text,
				base_dir,
				display_name,
				[&]() { return compile_mml_text(text, base_dir, display_name); },
				out_compile);
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
