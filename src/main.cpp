#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <audio/AudioStream.h>

#include "audio_output.h"
#include "highlight_tracker.h"
#include "mml_compile.h"
#include "vgm_export.h"
#include "wav_export.h"
#include "vgm_audio_renderer.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace
{
	std::atomic<bool> g_stop_requested(false);

	std::filesystem::path pid_path()
	{
		return std::filesystem::temp_directory_path() / "ctrmml-cmd.pid";
	}

	void write_pid()
	{
		std::ofstream out(pid_path());
		if (!out)
			return;
#if defined(_WIN32)
		out << GetCurrentProcessId();
#else
		out << getpid();
#endif
	}

	void clear_pid()
	{
		std::error_code ec;
		std::filesystem::remove(pid_path(), ec);
	}

	bool parse_line_col(const std::string &value, uint32_t &line, uint32_t &col)
	{
		auto pos = value.find(':');
		if (pos == std::string::npos)
			return false;
		try
		{
			line = static_cast<uint32_t>(std::stoul(value.substr(0, pos)));
			col = static_cast<uint32_t>(std::stoul(value.substr(pos + 1)));
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	void print_usage()
	{
		std::cerr << "Usage:\n"
							<< "  ctrmml-cmd play <file> [--start line:col] [--follow]\n"
							<< "  ctrmml-cmd stop\n"
							<< "  ctrmml-cmd check <file>\n"
							<< "  ctrmml-cmd export <file> --vgm|--wav [--out path]\n";
	}

#if !defined(_WIN32)
	void handle_signal(int)
	{
		g_stop_requested.store(true);
	}
#endif

	bool stop_running_process()
	{
		std::ifstream in(pid_path());
		if (!in)
			return false;
		long long pid = 0;
		in >> pid;
		if (!pid)
			return false;

#if defined(_WIN32)
		HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
		if (!h)
			return false;
		BOOL ok = TerminateProcess(h, 0);
		CloseHandle(h);
		return ok != 0;
#else
		return kill(static_cast<pid_t>(pid), SIGTERM) == 0;
#endif
	}

	void emit_highlight(uint32_t ticks, const std::vector<HighlightPosition> &positions)
	{
		std::cout << "{\"type\":\"highlight\",\"ticks\":" << ticks << ",\"positions\":[";
		for (size_t i = 0; i < positions.size(); ++i)
		{
			if (i)
				std::cout << ',';
			std::cout << "{\"line\":" << positions[i].line << ",\"col\":" << positions[i].col << "}";
		}
		std::cout << "]}\n"
							<< std::flush;
	}

}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		print_usage();
		return 1;
	}

	std::string cmd = argv[1];
	if (cmd == "stop")
	{
		if (!stop_running_process())
		{
			std::cerr << "no running process\n";
			return 1;
		}
		return 0;
	}

	if (argc < 3)
	{
		print_usage();
		return 1;
	}

	std::string file = argv[2];

	if (cmd == "check")
	{
		bool use_stdin = (file == "--stdin" || file == "-");
		if (use_stdin)
		{
			std::string display_path;
			for (int i = 3; i < argc; ++i)
			{
				std::string arg = argv[i];
				if (arg == "--path" && i + 1 < argc)
					display_path = argv[++i];
			}
			std::ostringstream buffer;
			buffer << std::cin.rdbuf();
			std::string input = buffer.str();
			std::string base_dir = std::filesystem::current_path().string();
			std::string display_name = "<stdin>";
			if (!display_path.empty())
			{
				std::filesystem::path display_fs = std::filesystem::absolute(display_path);
				base_dir = display_fs.parent_path().string();
				display_name = display_fs.string();
			}
			auto compile = compile_mml_text(input, base_dir, display_name);
			if (!compile.song)
			{
				std::cerr << compile.error << std::endl;
				return 1;
			}
			return 0;
		}
		auto compile = compile_mml_file(file);
		if (!compile.song)
		{
			std::cerr << compile.error << std::endl;
			return 1;
		}
		return 0;
	}

	if (cmd == "export")
	{
		std::string out_path;
		bool want_vgm = false;
		bool want_wav = false;
		bool use_stdin = (file == "--stdin" || file == "-");
		std::string display_path;
		for (int i = 3; i < argc; ++i)
		{
			std::string arg = argv[i];
			if (arg == "--vgm")
				want_vgm = true;
			else if (arg == "--wav")
				want_wav = true;
			else if (arg == "--out" && i + 1 < argc)
				out_path = argv[++i];
			else if (arg == "--path" && i + 1 < argc)
				display_path = argv[++i];
		}

		std::filesystem::path path_for_out = use_stdin && !display_path.empty()
			? std::filesystem::absolute(display_path)
			: std::filesystem::path(file);
		if (out_path.empty())
			out_path = path_for_out.replace_extension(want_wav ? ".wav" : ".vgm").string();

		if (use_stdin)
		{
			std::ostringstream buffer;
			buffer << std::cin.rdbuf();
			std::string input = buffer.str();
			std::string base_dir = std::filesystem::current_path().string();
			std::string display_name = "<stdin>";
			if (!display_path.empty())
			{
				std::filesystem::path display_fs = std::filesystem::absolute(display_path);
				base_dir = display_fs.parent_path().string();
				display_name = display_fs.string();
			}
			if (want_wav)
				return export_wav_text(input, base_dir, display_name, out_path) ? 0 : 1;
			return export_vgm_text(input, base_dir, display_name, out_path) ? 0 : 1;
		}

		if (want_wav)
			return export_wav(file, out_path) ? 0 : 1;
		return export_vgm(file, out_path) ? 0 : 1;
	}

	if (cmd == "play")
	{
		uint32_t start_line = 0;
		uint32_t start_col = 0;
		bool has_start = false;
		bool follow = false;
		bool use_stdin = (file == "--stdin" || file == "-");
		std::string display_path;
		for (int i = 3; i < argc; ++i)
		{
			std::string arg = argv[i];
			if (arg == "--start" && i + 1 < argc)
			{
				has_start = parse_line_col(argv[++i], start_line, start_col);
			}
			else if (arg == "--follow")
			{
				follow = true;
			}
			else if (arg == "--path" && i + 1 < argc)
			{
				display_path = argv[++i];
			}
		}

		CompileResult compile{};
		if (use_stdin)
		{
			std::ostringstream buffer;
			buffer << std::cin.rdbuf();
			std::string input = buffer.str();
			std::string base_dir = std::filesystem::current_path().string();
			std::string display_name = "<stdin>";
			if (!display_path.empty())
			{
				std::filesystem::path display_fs = std::filesystem::absolute(display_path);
				base_dir = display_fs.parent_path().string();
				display_name = display_fs.string();
			}
			compile = compile_mml_text(input, base_dir, display_name);
		}
		else
		{
			compile = compile_mml_file(file);
		}
		if (!compile.song)
		{
			std::cerr << compile.error << std::endl;
			return 1;
		}

		uint32_t start_ticks = 0;
		if (has_start && compile.tracks)
			start_ticks = find_start_ticks(*compile.tracks, start_line, start_col);

		VgmAudioRenderer renderer(compile.song, start_ticks);
		renderer.setup_stream(44100);

#if !defined(_WIN32)
		signal(SIGINT, handle_signal);
		signal(SIGTERM, handle_signal);
#endif

		AudioOutput output;
		if (!output.start(&renderer, 44100))
		{
			std::cerr << "audio output failed\n";
			return 1;
		}

		write_pid();

		uint32_t last_ticks = 0xffffffffu;
		while (!renderer.is_finished() && !g_stop_requested.load())
		{
			if (follow && compile.tracks)
			{
				auto driver = renderer.get_driver();
				uint32_t ticks = driver ? driver->get_player_ticks() : 0;
				if (ticks != last_ticks)
				{
					last_ticks = ticks;
					auto positions = collect_highlights(*compile.tracks, ticks, 64);
					emit_highlight(ticks, positions);
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		renderer.stop_playback();
		output.stop();
		clear_pid();
		return 0;
	}

	print_usage();
	return 1;
}
