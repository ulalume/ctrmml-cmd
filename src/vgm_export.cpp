#include "vgm_export.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include "input.h"
#include "mml_compile.h"

namespace
{
	ExportResult export_vgm_song(const std::shared_ptr<Song> &song, const std::string &out_path)
	{
		try
		{
			// Unlike VgmAudioRenderer::get_sample(), the library VGM render loop
			// does not catch driver errors. Convert them here before they can cross
			// the wasm boundary as Emscripten's opaque CppException object.
			std::vector<uint8_t> data = song->get_platform()->get_export_data(*song, 0);
			std::ofstream out(out_path, std::ios::binary);
			if (!out)
				return {false, "failed to open output file"};
			out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
			if (!out)
			{
				out.close();
				std::remove(out_path.c_str());
				return {false, "failed to write output file"};
			}
			return {true, {}};
		}
		catch (InputError &e)
		{
			return {false, e.what()};
		}
		catch (std::exception &e)
		{
			return {false, e.what()};
		}
	}
}

ExportResult export_vgm(const std::string &in_path, const std::string &out_path)
{
	auto compile = compile_mml_file(in_path);
	if (!compile.song)
	{
		return {false, compile.error};
	}
	return export_vgm_song(compile.song, out_path);
}

ExportResult export_vgm_text(const std::string &text, const std::string &base_path, const std::string &display_name, const std::string &out_path)
{
	auto compile = compile_mml_text(text, base_path, display_name);
	if (!compile.song)
	{
		return {false, compile.error};
	}
	return export_vgm_song(compile.song, out_path);
}
