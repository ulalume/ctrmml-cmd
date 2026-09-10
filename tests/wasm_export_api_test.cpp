#include <cstdint>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ctrmml_cmd_wasm_api.h"

namespace
{
	std::string read_file(const std::filesystem::path &path)
	{
		std::ifstream in(path);
		std::ostringstream contents;
		contents << in.rdbuf();
		return contents.str();
	}

	bool expect_failure(const char *name,
								 int (*export_fn)(const char *, const char *, const char *),
								 const std::string &text,
								 const std::filesystem::path &base_dir,
								 const std::filesystem::path &output)
	{
		std::filesystem::remove(output);
		int rc = export_fn(text.c_str(), base_dir.string().c_str(), output.string().c_str());
		std::string error = ctrmml_cmd_wasm_get_last_error();
		if (rc == 0 || error.find("Panning not supported for PSG channels") == std::string::npos || std::filesystem::exists(output))
		{
			std::cerr << name << " failure contract mismatch: rc=" << rc
							<< ", error=" << error
							<< ", output_exists=" << std::filesystem::exists(output) << '\n';
			return false;
		}
		return true;
	}

	bool expect_success(const char *name,
								 int (*export_fn)(const char *, const char *, const char *),
								 const std::string &text,
								 const std::filesystem::path &base_dir,
								 const std::filesystem::path &output)
	{
		std::filesystem::remove(output);
		int rc = export_fn(text.c_str(), base_dir.string().c_str(), output.string().c_str());
		std::string error = ctrmml_cmd_wasm_get_last_error();
		if (rc != 0 || !error.empty() || !std::filesystem::exists(output))
		{
			std::cerr << name << " success contract mismatch: rc=" << rc
							<< ", error=" << error
							<< ", output_exists=" << std::filesystem::exists(output) << '\n';
			return false;
		}
		return true;
	}
}

namespace
{
	void write_test_wav(const std::filesystem::path &path, uint32_t rate, uint32_t frames)
	{
		std::vector<uint8_t> data(frames);
		for (uint32_t i = 0; i < frames; ++i)
			data[i] = ((i / 20) & 1) ? 0xc0 : 0x40;
		auto u32 = [](std::ofstream &f, uint32_t v) { f.put(v & 0xff); f.put((v >> 8) & 0xff); f.put((v >> 16) & 0xff); f.put((v >> 24) & 0xff); };
		auto u16 = [](std::ofstream &f, uint16_t v) { f.put(v & 0xff); f.put((v >> 8) & 0xff); };
		std::ofstream f(path, std::ios::binary);
		f.write("RIFF", 4); u32(f, 36 + frames); f.write("WAVE", 4);
		f.write("fmt ", 4); u32(f, 16); u16(f, 1); u16(f, 1); u32(f, rate); u32(f, rate); u16(f, 1); u16(f, 8);
		f.write("data", 4); u32(f, frames);
		f.write(reinterpret_cast<const char *>(data.data()), frames);
	}

	// Renders the compiled song until it stops or `limit_frames` pass.
	uint32_t render_until_stopped(uint32_t limit_frames)
	{
		std::vector<float> buffer(1024 * 2);
		uint32_t total = 0;
		while (total < limit_frames && ctrmml_cmd_wasm_is_playing())
		{
			int written = ctrmml_cmd_wasm_render_audio(buffer.data(), 1024);
			if (written <= 0)
				break;
			total += static_cast<uint32_t>(written);
		}
		return total;
	}

	bool test_stop_at_pcm_end(const std::filesystem::path &dir, const char *platform)
	{
		constexpr uint32_t kRate = 44100;
		std::filesystem::create_directories(dir);
		const auto wav = dir / "stop-at-pcm-end.wav";
		write_test_wav(wav, 17500, 1750); // 100 ms
		const std::string text = std::string("#platform ") + platform + "\n@1 pcm \"stop-at-pcm-end.wav\"\nF @1 o4 l1 c^1^1^1\n";
		bool ok = true;
		if (ctrmml_cmd_wasm_compile(text.c_str(), dir.string().c_str()) != 0)
		{
			std::cerr << "stop-at-pcm-end compile failed: " << ctrmml_cmd_wasm_get_compile_error() << '\n';
			std::filesystem::remove(wav);
			return false;
		}

		ok &= ctrmml_cmd_wasm_start_playback(kRate, 0, 0) == 0;
		ctrmml_cmd_wasm_set_stop_at_pcm_end(1);
		const uint32_t stopped = render_until_stopped(kRate * 2);
		std::cout << "stop-at-pcm-end (" << platform << "): stopped after " << stopped * 1000 / kRate << " ms\n";
		if (stopped < kRate / 10 || stopped > kRate / 4)
		{
			std::cerr << "expected playback to stop shortly after the 100 ms sample\n";
			ok = false;
		}

		ok &= ctrmml_cmd_wasm_start_playback(kRate, 0, 0) == 0;
		const uint32_t held = render_until_stopped(kRate);
		if (held < kRate || !ctrmml_cmd_wasm_is_playing())
		{
			std::cerr << "a new playback should hold the note without the flag\n";
			ok = false;
		}
		ctrmml_cmd_wasm_stop_playback();
		std::filesystem::remove(wav);
		return ok;
	}
}

int main(int argc, char **argv)
{
	if (argc != 4)
	{
		std::cerr << "usage: wasm_export_api_test BAD_FIXTURE CLEAN_FIXTURE OUTPUT_DIR\n";
		return 2;
	}

	std::filesystem::path bad_fixture = argv[1];
	std::filesystem::path clean_fixture = argv[2];
	std::filesystem::path output_dir = argv[3];
	std::filesystem::create_directories(output_dir);
	std::string bad_text = read_file(bad_fixture);
	std::string clean_text = read_file(clean_fixture);
	std::filesystem::path base_dir = bad_fixture.parent_path();

	bool ok = true;
	ok &= expect_failure("wasm WAV", ctrmml_cmd_wasm_export_wav, bad_text, base_dir, output_dir / "wasm-bad.wav");
	ok &= expect_failure("wasm VGM", ctrmml_cmd_wasm_export_vgm, bad_text, base_dir, output_dir / "wasm-bad.vgm");
	ok &= expect_success("wasm WAV", ctrmml_cmd_wasm_export_wav, clean_text, base_dir, output_dir / "wasm-clean.wav");
	ok &= expect_success("wasm VGM", ctrmml_cmd_wasm_export_vgm, clean_text, base_dir, output_dir / "wasm-clean.vgm");
	ctrmml_cmd_wasm_set_ym2612_chip_type(1);
	ok &= expect_success("wasm YM3438 WAV", ctrmml_cmd_wasm_export_wav, clean_text, base_dir, output_dir / "wasm-clean-ym3438.wav");
	ok &= test_stop_at_pcm_end(output_dir, "megadrive");
	ok &= test_stop_at_pcm_end(output_dir, "mdsdrv");
	ctrmml_cmd_wasm_set_ym2612_chip_type(0);

	std::filesystem::remove(output_dir / "wasm-bad.wav");
	std::filesystem::remove(output_dir / "wasm-bad.vgm");
	std::filesystem::remove(output_dir / "wasm-clean.wav");
	std::filesystem::remove(output_dir / "wasm-clean.vgm");
	std::filesystem::remove(output_dir / "wasm-clean-ym3438.wav");
	return ok ? 0 : 1;
}
