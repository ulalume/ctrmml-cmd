#pragma once

#include <string>
#include <vector>

#include "ctrmml_cmd.h"

class Song;

namespace ctrmml_cmd
{
	std::vector<CheckMessage> collect_note_range_warnings(
			Song &song,
			const std::string &display_name);
}
