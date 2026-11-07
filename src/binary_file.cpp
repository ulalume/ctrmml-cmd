#include "binary_file.h"

#include <cstdio>
#include <fstream>

bool write_binary_file(const std::string &path, const std::vector<uint8_t> &bytes, std::string &error)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
	{
		error = "failed to open output file: " + path;
		return false;
	}
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	out.close();
	if (!out)
	{
		std::remove(path.c_str());
		error = "failed to write output file: " + path;
		return false;
	}
	return true;
}
