#pragma once

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

MdslinkResult run_mdslink(const MdslinkOptions& options);
