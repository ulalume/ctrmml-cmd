// Renders MML files from the compiled song and from the song after
// pack_playback_song()/unpack_playback_song(), and requires the same WAV, or
// the same error text for a song that fails during playback.
//
//   playback-song-test <file.mml>...

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "mml_compile.h"
#include "playback_song.h"
#include "song.h"
#include "wav_export.h"

namespace
{
	bool check_file(const std::string &path, const std::string &empty_dir)
	{
		CompileResult compile = compile_mml_file(path);
		if (!compile.song)
		{
			std::cerr << "failed: " << path << ": " << compile.error << '\n';
			return false;
		}

		std::shared_ptr<Song> copy;
		try
		{
			std::vector<uint8_t> packed = pack_playback_song(*compile.song);
			copy = unpack_playback_song(packed.data(), packed.size());
		}
		catch (std::exception &e)
		{
			std::cerr << "failed: " << path << ": packing: " << e.what() << '\n';
			return false;
		}
		copy->get_tag_map()["include_path"] = {empty_dir};

		std::vector<uint8_t> direct;
		std::vector<uint8_t> via_pack;
		ExportResult direct_result = render_wav(compile.song, direct);
		ExportResult pack_result = render_wav(copy, via_pack);
		if (direct_result.ok != pack_result.ok || direct_result.error != pack_result.error)
		{
			std::cerr << "differs: " << path << ": direct: "
								<< (direct_result.ok ? "ok" : direct_result.error) << ", packed: "
								<< (pack_result.ok ? "ok" : pack_result.error) << '\n';
			return false;
		}
		if (direct != via_pack)
		{
			std::cerr << "differs: " << path << '\n';
			return false;
		}
		std::cout << "ok: " << path << (direct_result.ok ? "" : " (" + direct_result.error + ")") << '\n';
		return true;
	}
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: playback-song-test <file.mml>...\n";
		return 2;
	}

	std::filesystem::path empty_dir = std::filesystem::temp_directory_path() /
			("playback-song-test-" + std::to_string(std::random_device{}()));
	std::filesystem::create_directories(empty_dir);

	bool ok = true;
	for (int i = 1; i < argc; i++)
		ok &= check_file(argv[i], (empty_dir / "").string());

	std::filesystem::remove_all(empty_dir);
	return ok ? 0 : 1;
}
