// Plays songs through the Playback class of the player module.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "mml_compile.h"
#include "playback.h"
#include "song.h"
#include "song_blob.h"

namespace
{
	constexpr uint32_t kRate = 44100;

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

	std::shared_ptr<Song> compile(const std::string &text, const std::filesystem::path &dir)
	{
		CompileResult result = compile_mml_text(text, dir.string(), "input.mml");
		if (!result.song)
			std::cerr << "compile failed: " << result.error << '\n';
		return result.song;
	}

	// Renders until playback stops or `limit_frames` pass.
	uint32_t render_until_stopped(Playback &playback, uint32_t limit_frames)
	{
		std::vector<float> left(1024), right(1024);
		uint32_t total = 0;
		while (total < limit_frames && playback.is_playing())
		{
			playback.render(left.data(), right.data(), 1024);
			total += 1024;
		}
		return total;
	}

	bool test_stop_at_pcm_end(const std::filesystem::path &dir, const char *platform)
	{
		write_test_wav(dir / "stop-at-pcm-end.wav", 17500, 1750); // 100 ms
		auto song = compile(std::string("#platform ") + platform +
				"\n@1 pcm \"stop-at-pcm-end.wav\"\nF @1 o4 l1 c^1^1^1\n", dir);
		if (!song)
			return false;
		bool ok = true;
		Playback playback;
		playback.init(kRate);
		ok &= playback.load(song, 0);
		playback.set_stop_at_pcm_end(true);
		const uint32_t stopped = render_until_stopped(playback, kRate * 2);
		std::cout << "stop-at-pcm-end (" << platform << "): stopped after " << stopped * 1000 / kRate << " ms\n";
		if (stopped < kRate / 10 || stopped > kRate / 4)
		{
			std::cerr << "expected playback to stop shortly after the 100 ms sample\n";
			ok = false;
		}

		ok &= playback.load(song, 0);
		const uint32_t held = render_until_stopped(playback, kRate);
		if (held < kRate || !playback.is_playing())
		{
			std::cerr << "a new playback should hold the note without the flag\n";
			ok = false;
		}
		return ok;
	}

	// A sample that is missing when the song plays fails with its InputError
	// text.
	bool test_missing_sample_error(const std::filesystem::path &dir)
	{
		auto song = compile("#platform megadrive\n@1 pcm \"missing.wav\"\nF @1 c\n", dir);
		if (!song)
			return false;
		const std::vector<uint8_t> blob = serialize_song(*song, true);
		Playback playback;
		playback.init(kRate);
		bool ok = !playback.load(blob.data(), blob.size(), 0);
		ok &= playback.last_error() == "missing.wav not found";
		if (!ok)
			std::cerr << "a missing sample should fail to load with its text, got \"" << playback.last_error() << "\"\n";
		return ok;
	}

	// A relink without a playing song leaves the data unread.
	bool test_relink_when_stopped(const std::filesystem::path &dir)
	{
		Playback playback;
		playback.init(kRate);
		const uint8_t garbage[4] = {1, 2, 3, 4};
		bool ok = playback.relink(garbage, sizeof(garbage)) && playback.last_error().empty();

		auto song = compile("#platform megadrive\n@1 psg 15 /\nG @1 c1\n", dir);
		ok &= song && playback.load(song, 0);
		ok &= !playback.relink(garbage, sizeof(garbage)) && playback.is_playing();
		if (!ok)
			std::cerr << "a relink should read the song only while one plays\n";
		return ok;
	}
}

int main()
{
	std::filesystem::path dir = std::filesystem::temp_directory_path() /
			("playback-test-" + std::to_string(std::random_device{}()));
	std::filesystem::create_directories(dir);
	bool ok = true;
	ok &= test_stop_at_pcm_end(dir, "megadrive");
	ok &= test_stop_at_pcm_end(dir, "mdsdrv");
	ok &= test_missing_sample_error(dir);
	ok &= test_relink_when_stopped(dir);
	std::filesystem::remove_all(dir);
	return ok ? 0 : 1;
}
