#include "mdslink_tool.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>

#include "input.h"
#include "mml_compile.h"
#include "platform/mdsdrv.h"
#include "stringf.h"

namespace
{
	std::string file_extension(const std::string& input_filename)
	{
		auto pos = input_filename.rfind('.');
		if (pos != std::string::npos)
			return input_filename.substr(pos);
		return "";
	}

	std::string file_stem(const std::string& input_filename)
	{
		std::filesystem::path path(input_filename);
		auto stem = path.stem().string();
		if (!stem.empty())
			return stem;
		auto name = path.filename().string();
		return name.empty() ? input_filename : name;
	}

	MdslinkResult load_mds_file(const std::string& path, RIFF& out)
	{
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in)
		return {false, stringf("Couldn't open %s", path.c_str())};
	auto size = in.tellg();
	if (size <= 0)
		return {false, stringf("Couldn't read %s", path.c_str())};
	auto count = static_cast<std::streamsize>(size);
	std::vector<uint8_t> data(static_cast<size_t>(size), 0);
	in.seekg(0);
	if (!in.read(reinterpret_cast<char*>(data.data()), count))
		return {false, stringf("Couldn't read %s", path.c_str())};
		out = RIFF(data);
		return {true, ""};
	}

	MdslinkResult convert_mml_file(const std::string& path, RIFF& out)
	{
		auto compile = compile_mml_file(path);
		if (!compile.song)
			return {false, compile.error.empty() ? "failed to compile mml" : compile.error};
		auto converter = MDSDRV_Converter(*compile.song);
		out = converter.get_mds();
		return {true, ""};
	}

	MdslinkResult write_binary_file(const std::string& path, const std::vector<uint8_t>& bytes)
	{
		std::ofstream out(path, std::ios::binary);
		if (!out)
			return {false, stringf("Couldn't write %s", path.c_str())};
		out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		return {true, ""};
	}
}

MdslinkResult run_mdslink(const MdslinkOptions& options)
{
	auto build = build_mdslink_payload(options);
	if (!build.ok)
		return {false, build.error};

	if (!options.seq_output.empty())
	{
		std::cout << "writing " << options.seq_output << " ...\n";
		auto result = write_binary_file(options.seq_output, build.payload.seq_data);
		if (!result.ok)
			return result;
	}
	if (!options.pcm_output.empty())
	{
		std::cout << "writing " << options.pcm_output << " ...\n";
		auto result = write_binary_file(options.pcm_output, build.payload.pcm_data);
		if (!result.ok)
			return result;
		std::cout << build.payload.statistics;
	}
	if (!options.asm_header_output.empty())
	{
		std::cout << "writing " << options.asm_header_output << " ...\n";
		std::ofstream out(options.asm_header_output);
		if (!out)
			return {false, stringf("Couldn't write %s", options.asm_header_output.c_str())};
		out.write(build.payload.asm_header.data(),
							static_cast<std::streamsize>(build.payload.asm_header.size()));
	}
	if (!options.c_header_output.empty())
	{
		std::cout << "writing " << options.c_header_output << " ...\n";
		std::ofstream out(options.c_header_output);
		if (!out)
			return {false, stringf("Couldn't write %s", options.c_header_output.c_str())};
		out.write(build.payload.c_header.data(),
							static_cast<std::streamsize>(build.payload.c_header.size()));
	}

	return {true, ""};
}

MdslinkBuildResult build_mdslink_payload(const MdslinkOptions& options)
{
	if (options.inputs.empty())
		return {false, "no input specified", {}};

	try
	{
		auto linker = MDSDRV_Linker();
		for (size_t index = 0; index < options.inputs.size(); ++index)
		{
			const auto& input_path = options.inputs[index];
			auto extension = file_extension(input_path);
			RIFF mds(0);
			std::cout << "[" << (index + 1) << "/" << options.inputs.size() << "] "
								<< input_path << "\n";

			if (iequal(extension, ".mds"))
			{
				auto result = load_mds_file(input_path, mds);
				if (!result.ok)
					return {false, result.error, {}};
			}
			else
			{
				auto result = convert_mml_file(input_path, mds);
				if (!result.ok)
					return {false, result.error, {}};
			}

			linker.add_song(mds, file_stem(input_path));
		}

		MdslinkPayload payload{};
		payload.seq_data = linker.get_seq_data();
		payload.pcm_data = linker.get_pcm_data();
		payload.statistics = linker.get_statistics();
		if (!options.asm_header_output.empty())
			payload.asm_header = linker.get_asm_header();
		if (!options.c_header_output.empty())
			payload.c_header = linker.get_c_header();
		return {true, "", std::move(payload)};
	}
	catch (InputError& error)
	{
		return {false, error.what(), {}};
	}
	catch (std::exception& error)
	{
		return {false, error.what(), {}};
	}
}
