#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MdslinkOptions
{
	std::vector<std::string> inputs;
	std::string seq_output = "mdsseq.bin";
	std::string pcm_output = "mdspcm.bin";
	std::string asm_header_output;
	std::string c_header_output;
};

struct MdslinkResult
{
	bool ok = false;
	std::string error;
};

struct MdslinkPayload
{
	std::vector<uint8_t> seq_data;
	std::vector<uint8_t> pcm_data;
	std::string asm_header;
	std::string c_header;
	std::string statistics;
	unsigned int song_count = 0;
};

struct MdslinkBuildResult
{
	bool ok = false;
	std::string error;
	MdslinkPayload payload;
};

MdslinkBuildResult build_mdslink_payload(const MdslinkOptions& options);
MdslinkResult write_mdslink_outputs(const MdslinkOptions& options, const MdslinkPayload& payload);
MdslinkResult run_mdslink(const MdslinkOptions& options);

//! Format a human-readable build summary (song count + seq/pcm sizes),
//! mirroring the style of format_rom_build_summary().
std::string format_mdslink_build_summary(const MdslinkPayload& payload);
