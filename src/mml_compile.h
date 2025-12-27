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
	std::string error;
};

CompileResult compile_mml_file(const std::string& path);
