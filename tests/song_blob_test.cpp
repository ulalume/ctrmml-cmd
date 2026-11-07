// Renders MML files to WAV from the compiled song and from the song after
// serialize_song()/deserialize_song(), and requires identical output.
//
//   song-blob-test [--write-dir <dir>] <file.mml>...
//
// The deserialized song gets an empty directory as its include path, so its
// PCM samples can only come from the serialized data. With --write-dir, the
// WAVs rendered from the deserialized songs are written to <dir>, named after
// the input path with '/' and ' ' replaced by '_', plus ".wav".

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "binary_file.h"
#include "mml_compile.h"
#include "song.h"
#include "song_blob.h"
#include "wav_export.h"

namespace
{
	std::string output_name(const std::string &path)
	{
		std::string name = path;
		for (char &c : name)
		{
			if (c == '/' || c == ' ')
				c = '_';
		}
		return name + ".wav";
	}

	bool check_file(const std::string &path, const std::string &empty_dir, const std::string &write_dir)
	{
		CompileResult compile = compile_mml_file(path);
		if (!compile.song)
		{
			std::cerr << "failed: " << path << ": " << compile.error << '\n';
			return false;
		}

		auto copy = std::make_shared<Song>();
		try
		{
			std::vector<uint8_t> blob = serialize_song(*compile.song);
			deserialize_song(blob.data(), blob.size(), *copy);
		}
		catch (std::exception &e)
		{
			std::cerr << "failed: " << path << ": serialization: " << e.what() << '\n';
			return false;
		}
		copy->get_tag_map()["include_path"] = {empty_dir};

		std::vector<uint8_t> direct;
		std::vector<uint8_t> via_blob;
		ExportResult direct_result = render_wav(compile.song, direct);
		ExportResult blob_result = render_wav(copy, via_blob);
		if (!direct_result.ok || !blob_result.ok)
		{
			std::cerr << "failed: " << path << ": direct: "
								<< (direct_result.ok ? "ok" : direct_result.error) << ", via blob: "
								<< (blob_result.ok ? "ok" : blob_result.error) << '\n';
			return false;
		}
		if (!write_dir.empty())
		{
			std::string error;
			std::string out = (std::filesystem::path(write_dir) / output_name(path)).string();
			if (!write_binary_file(out, via_blob, error))
			{
				std::cerr << "failed: " << path << ": " << error << '\n';
				return false;
			}
		}
		if (direct != via_blob)
		{
			std::cerr << "differs: " << path << '\n';
			return false;
		}
		std::cout << "ok: " << path << '\n';
		return true;
	}
}

int main(int argc, char **argv)
{
	std::string write_dir;
	std::vector<std::string> files;
	for (int i = 1; i < argc; i++)
	{
		std::string arg = argv[i];
		if (arg == "--write-dir" && i + 1 < argc)
			write_dir = argv[++i];
		else
			files.push_back(arg);
	}
	if (files.empty())
	{
		std::cerr << "usage: song-blob-test [--write-dir <dir>] <file.mml>...\n";
		return 2;
	}

	std::filesystem::path empty_dir = std::filesystem::temp_directory_path() /
			("song-blob-test-" + std::to_string(std::random_device{}()));
	std::filesystem::create_directories(empty_dir);

	bool ok = true;
	for (const std::string &file : files)
		ok &= check_file(file, (empty_dir / "").string(), write_dir);

	std::filesystem::remove_all(empty_dir);
	return ok ? 0 : 1;
}
