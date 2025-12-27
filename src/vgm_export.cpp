#include "vgm_export.h"

#include <fstream>
#include <iostream>
#include <vector>

#include "mml_compile.h"

bool export_vgm(const std::string& in_path, const std::string& out_path)
{
	auto compile = compile_mml_file(in_path);
	if (!compile.song)
	{
		std::cerr << compile.error << "\n";
		return false;
	}

	std::vector<uint8_t> data = compile.song->get_platform()->get_export_data(*compile.song.get(), 0);
	std::ofstream out(out_path, std::ios::binary);
	if (!out)
	{
		std::cerr << "failed to open output file\n";
		return false;
	}
	out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	return true;
}
