#pragma once

#include <string>

#include "export_result.h"
#include "ymfm_ym2612_device.h"

ExportResult export_wav(const std::string &in_path, const std::string &out_path,
		Ym2612ChipType chip_type = Ym2612ChipType::Ym2612);
ExportResult export_wav_text(const std::string &text, const std::string &base_path,
		const std::string &display_name, const std::string &out_path,
		Ym2612ChipType chip_type = Ym2612ChipType::Ym2612);
