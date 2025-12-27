#include "mml_compile.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "mml_input.h"

namespace
{
	std::string tabs_to_spaces(const std::string& str)
	{
		const unsigned int tabstop = 4;
		std::string out;
		for (char c : str)
		{
			if (c == '\t')
			{
				do
				{
					out.push_back(' ');
				}
				while (out.size() % tabstop != 0);
			}
			else
			{
				out.push_back(c);
			}
		}
		return out;
	}

	std::string path_with_trailing_sep(const std::filesystem::path& path)
	{
		std::string value = path.string();
		if (!value.empty())
		{
			char last = value.back();
			if (last != '/' && last != '\\')
				value.push_back(std::filesystem::path::preferred_separator);
		}
		return value;
	}
}

CompileResult compile_mml_file(const std::string& path)
{
	CompileResult result{};
	try
	{
		auto song = std::make_shared<Song>();
		auto tracks = std::make_shared<std::map<int, TrackInfo>>();

		std::filesystem::path base = std::filesystem::absolute(path).parent_path();
		song->add_tag("include_path", path_with_trailing_sep(base));
		song->add_tag("include_path", path_with_trailing_sep(base / "pcm"));

		MML_Input input(song.get());

		std::ifstream in(path);
		if (!in)
		{
			result.error = "failed to open file";
			return result;
		}

		std::string line;
		int line_no = 0;
		while (std::getline(in, line))
		{
			try
			{
				input.read_line(tabs_to_spaces(line), line_no);
			}
			catch (std::exception& e)
			{
				result.error = "line " + std::to_string(line_no + 1) + ": " + e.what() + " [" + line + "]";
				return result;
			}
			line_no++;
		}

		for (auto it = song->get_track_map().begin(); it != song->get_track_map().end(); ++it)
		{
			tracks->emplace(it->first, TrackInfoGenerator(*song, it->second));
		}

		result.song = song;
		result.tracks = tracks;
		return result;
	}
	catch (InputError& e)
	{
		result.error = e.what();
	}
	catch (std::exception& e)
	{
		result.error = e.what();
	}
	catch (...)
	{
		result.error = "unknown exception during compile";
	}

	return result;
}
