#include "mml_compile.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "mml_input.h"

namespace
{
	std::string tabs_to_spaces(const std::string &str)
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
				} while (out.size() % tabstop != 0);
			}
			else
			{
				out.push_back(c);
			}
		}
		return out;
	}

	std::string format_input_error(InputError &e, const std::string &filename)
	{
		auto ref = e.get_reference();
		std::string message = e.what();
		if (!message.empty() && message[0] == ':')
			message = filename + message;
		if (ref)
		{
			std::string caret(ref->get_column(), ' ');
			caret.push_back('^');
			const std::string &line = ref->get_line_contents();
			if (!line.empty())
				return message + "\n" + line + "\n" + caret;
		}
		return message;
	}

	std::string path_with_trailing_sep(const std::filesystem::path &path)
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

	// Return the maximum track ID (exclusive) for the song's platform.
	// Track IDs >= this value are subroutines / macros with independent
	// tick timelines and must not be included in TrackInfo.
	// Currently only MDSDRV is supported (16 channels: 6 FM + 4 PSG + 6 aux).
	// When additional platforms are added, use song.get_platform() to
	// return the correct value per platform.
	int get_platform_max_channels(const Song & /*song*/)
	{
		return 16;
	}
}

CompileResult compile_mml_internal(std::istream &in, const std::string &base_path, const std::string &display_name)
{
	CompileResult result{};
	try

	{
		auto song = std::make_shared<Song>();
		auto tracks = std::make_shared<std::map<int, TrackInfo>>();
		auto lines = std::make_shared<CompileLineMap>();

		std::filesystem::path base = std::filesystem::absolute(base_path);
		song->add_tag("include_path", path_with_trailing_sep(base));

		MML_Input input(song.get());

		std::string line;
		int line_no = 0;
		while (std::getline(in, line))

		{
			try

			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				input.read_line(tabs_to_spaces(line), line_no);
				lines->insert({line_no, input.get_track_map()});
			}
			catch (InputError &e)

			{
				result.error = format_input_error(e, display_name);
				return result;
			}
			catch (std::exception &e)

			{
				result.error = "line " + std::to_string(line_no + 1) + ": " + e.what() + " [" + line + "]";
				return result;
			}
			line_no++;
		}

		int max_channels = get_platform_max_channels(*song);
		for (auto it = song->get_track_map().begin(); it != song->get_track_map().end(); ++it)

		{
			if (it->first >= max_channels)
				break;
			tracks->emplace(it->first, TrackInfoGenerator(*song, it->second));
		}

		result.song = song;
		result.tracks = tracks;
		result.lines = lines;
		return result;
	}
	catch (InputError &e)

	{
		result.error = format_input_error(e, display_name);
	}
	catch (std::exception &e)

	{
		result.error = e.what();
	}
	catch (...)

	{
		result.error = "unknown exception during compile";
	}

	return result;
}

CompileResult compile_mml_file(const std::string &path)
{
	std::ifstream in(path);
	if (!in)

	{
		CompileResult result{};
		result.error = "failed to open file";
		return result;
	}
	std::filesystem::path base = std::filesystem::absolute(path).parent_path();
	return compile_mml_internal(in, base.string(), path);
}

CompileResult compile_mml_text(const std::string &text, const std::string &base_path, const std::string &display_name)
{
	std::istringstream in(text);
	return compile_mml_internal(in, base_path, display_name);
}
