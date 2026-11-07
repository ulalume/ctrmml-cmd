#include <cstdint>
#include <cstdlib>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ctrmml_cmd_wasm_api.h"
#include "song.h"
#include "song_blob.h"

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
	bool test_pack_song(const std::string &text, const std::filesystem::path &base_dir)
	{
		uint32_t size = 0;
		bool ok = ctrmml_cmd_wasm_compile(text.c_str(), base_dir.string().c_str()) == 0;
		uint8_t *bytes = ctrmml_cmd_wasm_pack_song(&size);
		ok &= bytes != nullptr && size > 0;
		if (bytes)
		{
			Song song;
			try
			{
				deserialize_song(bytes, size, song);
			}
			catch (std::exception &e)
			{
				std::cerr << "packed song: " << e.what() << '\n';
				ok = false;
			}
			ok &= !song.get_track_map().empty();
		}
		std::free(bytes);
		if (!ok)
			std::cerr << "pack_song should return the compiled song\n";
		return ok;
	}
}

namespace
{
	std::string fm_instrument(int id, int alg)
	{
		return "@" + std::to_string(id) + " fm " + std::to_string(alg) + " 0\n"
				"\t12,8,0,0,5,16,0,2,7,0\n"
				"\t8,4,0,4,4,8,0,2,6,0\n"
				"\t12,8,0,0,5,16,0,2,3,0\n"
				"\t8,4,0,4,4,8,0,2,2,0\n";
	}

	bool expect_cursor_instrument(uint32_t line, uint32_t col, int expected)
	{
		const std::string json = ctrmml_cmd_wasm_find_cursor_channel_json(line, col);
		const std::string field = "\"instrument_id\":" + std::to_string(expected) + ",";
		if (json.find(field) == std::string::npos)
		{
			std::cerr << "cursor " << line << ':' << col << " expected instrument " << expected
								<< ", got " << json << '\n';
			return false;
		}
		return true;
	}

	// data[24] is OP1's TL.
	bool expect_cursor_op1_tl(uint32_t line, uint32_t col, int expected)
	{
		const std::string json = ctrmml_cmd_wasm_find_cursor_channel_json(line, col);
		const auto data = json.find("\"data\":[");
		int value = -1;
		if (data != std::string::npos)
		{
			size_t pos = data + 8;
			for (int i = 0; i < 24 && pos != std::string::npos; ++i)
				pos = json.find(',', pos) + 1;
			value = std::atoi(json.c_str() + pos);
		}
		if (value != expected)
		{
			std::cerr << "cursor " << line << ':' << col << " expected OP1 TL " << expected
								<< ", got " << json << '\n';
			return false;
		}
		return true;
	}

	bool test_cursor_channel_instrument(const std::filesystem::path &dir)
	{
		const std::string text = "#platform megadrive\n" + fm_instrument(1, 4) + fm_instrument(2, 5) +
				fm_instrument(3, 7) +
				"A @1 c d e\n"  // line 16
				"A @2 f g a\n"  // line 17
				"B @1 c d @3 e\n"  // line 18
				"C @1 'tl1 20' c 'tl1 40' d\n"; // line 19
		if (ctrmml_cmd_wasm_compile(text.c_str(), dir.string().c_str()) != 0)
		{
			std::cerr << "cursor channel compile failed: " << ctrmml_cmd_wasm_get_compile_error() << '\n';
			return false;
		}
		bool ok = true;
		ok &= expect_cursor_instrument(16, 10, 1);
		ok &= expect_cursor_instrument(17, 10, 2);
		ok &= expect_cursor_instrument(18, 9, 1);
		ok &= expect_cursor_instrument(18, 13, 3);
		ok &= expect_cursor_op1_tl(19, 16, 20);
		ok &= expect_cursor_op1_tl(19, 26, 40);
		return ok;
	}

	bool test_instrument_data_follows_compile(const std::filesystem::path &dir)
	{
		const std::string with_two = "#platform megadrive\n" + fm_instrument(1, 4) + fm_instrument(2, 5) + "A @2 c\n";
		const std::string with_one = "#platform megadrive\n" + fm_instrument(1, 4) + "A @1 c\n";
		bool ok = true;
		ok &= ctrmml_cmd_wasm_compile(with_two.c_str(), dir.string().c_str()) == 0;
		ok &= std::string(ctrmml_cmd_wasm_get_instrument_data_json(2)) != "null";
		ok &= ctrmml_cmd_wasm_compile(with_one.c_str(), dir.string().c_str()) == 0;
		ok &= std::string(ctrmml_cmd_wasm_get_instrument_data_json(2)) == "null";
		ctrmml_cmd_wasm_check_json(with_two.c_str(), dir.string().c_str());
		ok &= std::string(ctrmml_cmd_wasm_get_instrument_data_json(2)) != "null";
		ok &= ctrmml_cmd_wasm_compile("A @", dir.string().c_str()) != 0;
		ok &= std::string(ctrmml_cmd_wasm_get_instrument_data_json(1)) == "null";
		if (!ok)
			std::cerr << "instrument data should follow the latest compile\n";
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
	ctrmml_cmd_wasm_set_ym2612_chip_type(0);
	ok &= test_pack_song(clean_text, base_dir);
	ok &= test_cursor_channel_instrument(output_dir);
	ok &= test_instrument_data_follows_compile(output_dir);

	std::filesystem::remove(output_dir / "wasm-bad.wav");
	std::filesystem::remove(output_dir / "wasm-bad.vgm");
	std::filesystem::remove(output_dir / "wasm-clean.wav");
	std::filesystem::remove(output_dir / "wasm-clean.vgm");
	std::filesystem::remove(output_dir / "wasm-clean-ym3438.wav");
	return ok ? 0 : 1;
}
