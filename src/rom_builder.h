#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mdslink_tool.h"

struct RomBuildOptions
{
	MdslinkOptions mdslink;
	std::vector<uint8_t> template_rom_bytes;
	std::string output_rom_path;
	uint8_t fill_value = 0x00;
	bool update_checksum = true;
};

struct RomBuildResult
{
	bool ok = false;
	std::string error;
	size_t seq_size = 0;
	size_t pcm_size = 0;
	uint32_t seq_offset = 0;
	uint32_t pcm_offset = 0;
	uint32_t seq_slot_size = 0;
	uint32_t pcm_slot_size = 0;
	bool used_template_marker = false;
	uint16_t bgm_min = 0;
	uint16_t bgm_max = 0;
	uint16_t se_min = 0;
	uint16_t se_max = 0;
};

RomBuildResult run_rom_build(const RomBuildOptions &options);
