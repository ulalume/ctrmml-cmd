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

	std::filesystem::remove(output_dir / "wasm-bad.wav");
	std::filesystem::remove(output_dir / "wasm-bad.vgm");
	std::filesystem::remove(output_dir / "wasm-clean.wav");
	std::filesystem::remove(output_dir / "wasm-clean.vgm");
	return ok ? 0 : 1;
}
