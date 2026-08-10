#pragma once

#include <string>
#include <vector>

#include "ctrmml_cmd.h"

class Song;

namespace ctrmml_cmd
{
	std::vector<CheckMessage> collect_channel_event_warnings(
			Song &song,
			const std::string &display_name);
}
