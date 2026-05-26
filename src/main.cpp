#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "audio_output.h"
#include "ctrmml_cmd.h"
#include "highlight_tracker.h"
#include "mdslink_tool.h"
#include "mml_compile.h"
#include "rom_builder.h"
#include "template_rom_data.h"
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

	// --------------------------------------------------------------------
	// Hot-reload framing
	//
	// In `--hot-reload` mode the LSP keeps stdin open and writes a stream
	// of length-prefixed updates so playback can be relinked mid-song
	// without restarting the chip emulation.
	//
	//   Frame layout: `UPDATE <bytes>\n<MML body of <bytes> bytes>`
	//                 (optional trailing newline is ignored)
	//   Sentinel:     `END\n`           — caller has no more updates
	//
	// Both writer and reader treat the body as raw bytes so MML containing
	// arbitrary whitespace, comments, and embedded `\n` is safe.
	enum class FrameOutcome { Update, End, Error };

	FrameOutcome read_framed_update(std::istream &in, std::string &out_mml)
	{
		out_mml.clear();
		std::string header;
		if (!std::getline(in, header))
			return FrameOutcome::End;
		if (!header.empty() && header.back() == '\r')
			header.pop_back();
		if (header == "END")
			return FrameOutcome::End;
		const std::string prefix = "UPDATE ";
		if (header.rfind(prefix, 0) != 0)
		{
			std::cerr << "hot-reload: expected `UPDATE <bytes>`, got `" << header << "`\n";
			return FrameOutcome::Error;
		}
		std::size_t bytes = 0;
		try
		{
			bytes = static_cast<std::size_t>(std::stoul(header.substr(prefix.size())));
		}
		catch (...)
		{
			std::cerr << "hot-reload: bad byte count in `" << header << "`\n";
			return FrameOutcome::Error;
		}
		out_mml.resize(bytes);
		if (bytes > 0)
		{
			in.read(&out_mml[0], static_cast<std::streamsize>(bytes));
			if (static_cast<std::size_t>(in.gcount()) != bytes)
			{
				std::cerr << "hot-reload: stdin closed mid-frame (expected " << bytes
								<< " bytes, got " << in.gcount() << ")\n";
				return FrameOutcome::Error;
			}
		}
		// Tolerate (but don't require) a trailing `\n`; ym2612_convert and the
		// LSP both emit one for readability.
		if (in.peek() == '\n')
			in.get();
		return FrameOutcome::Update;
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
							<< "  ctrmml-cmd play --stdin --path <path> [--follow] [--hot-reload]\n"
							<< "  ctrmml-cmd stop\n"
							<< "  ctrmml-cmd check [--json] <file>\n"
							<< "  ctrmml-cmd find-cursor-tick [--stdin --path <path>] [--line N --col M] <file>\n"
							<< "  ctrmml-cmd mdslink [options] <input files...>\n"
							<< "  ctrmml-cmd quickrom [--out rom.bin] <input files...>\n"
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
		bool use_stdin = false;
		bool json_output = false;
		std::string display_path;
		std::string target_file;
		for (int i = 2; i < argc; ++i)
		{
			std::string arg = argv[i];
			if (arg == "--path" && i + 1 < argc)
				display_path = argv[++i];
			else if (arg == "--json")
				json_output = true;
			else if (arg == "--stdin" || arg == "-")
				use_stdin = true;
			else if (target_file.empty())
				target_file = arg;
		}
		if (!use_stdin && target_file.empty())
		{
			print_usage();
			return 1;
		}
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
			if (json_output)
			{
				auto report = ctrmml_cmd::check_text_report(input, base_dir, display_name);
				std::cout << ctrmml_cmd::check_report_json(report) << std::endl;
				return report.ok() ? 0 : 1;
			}
			auto result = ctrmml_cmd::check_text(input, base_dir, display_name);
			if (!result.ok)
			{
				std::cerr << result.error << std::endl;
				return 1;
			}
			return 0;
		}
		if (json_output)
		{
			auto report = ctrmml_cmd::check_file_report(target_file);
			std::cout << ctrmml_cmd::check_report_json(report) << std::endl;
			return report.ok() ? 0 : 1;
		}
		auto result = ctrmml_cmd::check_file(target_file);
		if (!result.ok)
		{
			std::cerr << result.error << std::endl;
			return 1;
		}
		return 0;
	}

	if (cmd == "find-cursor-tick")
	{
		// Compile the source then report the cursor's playback tick at
		// (line, col), plus the song's PPQN. Output is always JSON:
		//   {"cursor_tick": <int>, "ppqn": <uint>}
		// or
		//   {"error": "<message>"}
		// The cursor_tick value is -1 when find_cursor_tick can't map
		// the position to any compiled event (e.g. cursor in a comment
		// or outside any track).
		//
		// Line and column are 0-based, matching the WASM API
		// (ctrmml_cmd_wasm_find_cursor_tick).
		bool use_stdin = false;
		std::string display_path;
		std::string target_file;
		uint32_t line = 0;
		uint32_t col = 0;
		for (int i = 2; i < argc; ++i)
		{
			std::string arg = argv[i];
			if (arg == "--path" && i + 1 < argc)
				display_path = argv[++i];
			else if (arg == "--stdin" || arg == "-")
				use_stdin = true;
			else if (arg == "--line" && i + 1 < argc)
				line = static_cast<uint32_t>(std::stoul(argv[++i]));
			else if (arg == "--col" && i + 1 < argc)
				col = static_cast<uint32_t>(std::stoul(argv[++i]));
			else if (target_file.empty())
				target_file = arg;
		}
		if (!use_stdin && target_file.empty())
		{
			std::cout << R"({"error":"no input file"})" << std::endl;
			return 1;
		}

		CompileResult compile;
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
			compile = compile_mml_file(target_file);
		}

		if (!compile.song || !compile.lines)
		{
			std::ostringstream err;
			err << R"({"error":")" << compile.error << R"("})";
			std::cout << err.str() << std::endl;
			return 1;
		}

		int32_t tick = find_cursor_tick(*compile.song, *compile.lines, line, col);
		uint32_t ppqn = compile.song->get_ppqn();
		std::cout << "{\"cursor_tick\":" << tick << ",\"ppqn\":" << ppqn << "}"
							<< std::endl;
		return 0;
	}

	if (cmd == "mdslink")
	{
		MdslinkOptions options;
		for (int i = 2; i < argc; ++i)
		{
			std::string arg = argv[i];
			if ((arg == "-o" || arg == "--output") && i + 2 < argc)
			{
				options.seq_output = argv[++i];
				options.pcm_output = argv[++i];
			}
			else if ((arg == "-h" || arg == "--c-header") && i + 1 < argc)
			{
				options.c_header_output = argv[++i];
			}
			else if ((arg == "-i" || arg == "--asm-header") && i + 1 < argc)
			{
				options.asm_header_output = argv[++i];
			}
			else
			{
				options.inputs.push_back(arg);
			}
		}

		auto result = run_mdslink(options);
		if (!result.ok)
		{
			if (!result.error.empty())
				std::cerr << result.error << std::endl;
			else
				std::cerr << "mdslink failed" << std::endl;
			return 1;
		}
		return 0;
	}

	if (cmd == "quickrom")
	{
		RomBuildOptions options;
		options.template_rom_bytes.assign(
				ctrmml_embedded::kTemplateRomData,
				ctrmml_embedded::kTemplateRomData + ctrmml_embedded::kTemplateRomSize);

		for (int i = 2; i < argc; ++i)
		{
			std::string arg = argv[i];
			if (arg == "--out" && i + 1 < argc)
			{
				options.output_rom_path = argv[++i];
			}
			else if (!arg.empty() && arg[0] == '-')
			{
				std::cerr << "unknown option: " << arg << "\n";
				return 1;
			}
			else
			{
				options.mdslink.inputs.push_back(arg);
			}
		}

		if (options.mdslink.inputs.empty())
		{
			print_usage();
			return 1;
		}

		if (options.output_rom_path.empty())
		{
			auto input_path = std::filesystem::path(options.mdslink.inputs.front());
			auto stem = input_path.stem().string();
			if (stem.empty())
				stem = "song";
			options.output_rom_path =
					(std::filesystem::current_path() / (stem + ".bin")).string();
		}

		auto result = run_rom_build(options);
		if (!result.ok)
		{
			std::cerr << result.error << std::endl;
			return 1;
		}

		std::cout << "wrote " << options.output_rom_path << "\n";

		std::error_code file_size_error;
		const auto rom_size = std::filesystem::file_size(options.output_rom_path, file_size_error);
		std::cout << format_rom_build_summary(result, file_size_error ? 0 : rom_size) << "\n";
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
		bool hot_reload = false;
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
			else if (arg == "--hot-reload")
			{
				hot_reload = true;
			}
			else if (arg == "--path" && i + 1 < argc)
			{
				display_path = argv[++i];
			}
		}

		// `--hot-reload` is meaningful only when reading MML over stdin —
		// nothing else gives the LSP a channel to push updates through.
		if (hot_reload && !use_stdin)
		{
			std::cerr << "ctrmml-cmd play: --hot-reload requires --stdin\n";
			return 1;
		}

		std::string base_dir = std::filesystem::current_path().string();
		std::string display_name = "<stdin>";
		if (use_stdin && !display_path.empty())
		{
			std::filesystem::path display_fs = std::filesystem::absolute(display_path);
			base_dir = display_fs.parent_path().string();
			display_name = display_fs.string();
		}

		CompileResult compile{};
		if (use_stdin && hot_reload)
		{
			// In hot-reload mode the first stdin frame carries the initial
			// MML; subsequent frames are processed by the background reader
			// (set up below, after we've built the renderer).
			std::string initial;
			FrameOutcome outcome = read_framed_update(std::cin, initial);
			if (outcome != FrameOutcome::Update)
			{
				std::cerr << "ctrmml-cmd play: hot-reload expected initial UPDATE frame\n";
				return 1;
			}
			compile = compile_mml_text(initial, base_dir, display_name);
		}
		else if (use_stdin)
		{
			std::ostringstream buffer;
			buffer << std::cin.rdbuf();
			compile = compile_mml_text(buffer.str(), base_dir, display_name);
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
		if (has_start && compile.song && compile.lines)
		{
			if (start_line > 0)
				--start_line;
			if (start_col > 0)
				--start_col;
			start_ticks = find_start_ticks(*compile.song, *compile.lines, start_line, start_col);
		}

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

		// Hot-reload: spin a background reader that pulls subsequent
		// UPDATE frames off stdin and hands them to the render loop via a
		// mutex-guarded queue. The reader exits on EOF / END / parse
		// error, after which `pending_done` flips and we stop trying.
		std::mutex pending_mu;
		std::optional<std::string> pending_mml;
		std::atomic<bool> pending_done{false};
		std::thread reader;
		if (hot_reload)
		{
			reader = std::thread([&]() {
				std::string body;
				while (!g_stop_requested.load())
				{
					FrameOutcome outcome = read_framed_update(std::cin, body);
					if (outcome != FrameOutcome::Update)
						break;
					{
						std::lock_guard<std::mutex> lock(pending_mu);
						pending_mml = body;
					}
				}
				pending_done.store(true);
			});
		}

		uint32_t last_ticks = 0xffffffffu;
		while (!renderer.is_finished() && !g_stop_requested.load())
		{
			if (hot_reload)
			{
				std::optional<std::string> next;
				{
					std::lock_guard<std::mutex> lock(pending_mu);
					if (pending_mml.has_value())
						next.swap(pending_mml);
				}
				if (next.has_value())
				{
					CompileResult fresh = compile_mml_text(*next, base_dir, display_name);
					if (fresh.song)
					{
						auto driver = renderer.get_driver();
						uint32_t cur = driver ? driver->get_player_ticks() : 0;
						try
						{
							renderer.relink_song(fresh.song, cur);
							compile = std::move(fresh);
						}
						catch (std::exception &e)
						{
							std::cerr << "hot-reload: relink failed: " << e.what() << "\n";
						}
					}
					else
					{
						// Compile errors leave the running renderer untouched so
						// the user can keep listening to the previous good
						// version while fixing the mistake.
						std::cerr << "hot-reload: compile failed: " << fresh.error
											<< "\n";
					}
				}
			}

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
		if (reader.joinable())
		{
			// Unblock the reader's `std::cin.read` so we can join cleanly
			// before the stack frame goes away. Detaching here would let
			// the thread outlive its captured locals (`pending_*`) and
			// the underlying mutex — a real use-after-free on shutdown.
#if defined(_WIN32)
			std::freopen("nul", "r", stdin);
#else
			std::freopen("/dev/null", "r", stdin);
#endif
			reader.join();
		}
		clear_pid();
		return 0;
	}

	print_usage();
	return 1;
}
