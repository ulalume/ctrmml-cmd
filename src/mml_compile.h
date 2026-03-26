#pragma once

#include <map>
#include <memory>
#include <string>

#include "song.h"
#include "track.h"

#include "highlight_tracker.h"

struct CompileResult
{
	std::shared_ptr<Song> song;
	std::shared_ptr<std::map<int, TrackInfo>> tracks;
	std::shared_ptr<CompileLineMap> lines;
	std::string error;
};

CompileResult compile_mml_file(const std::string& path);
CompileResult compile_mml_text(const std::string& text, const std::string& base_path, const std::string& display_name);
