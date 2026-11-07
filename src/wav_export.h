#pragma once

#include <memory>
#include <string>
#include <vector>

#include "export_result.h"
#include "song.h"
#include "ymfm_ym2612_device.h"

//! Renders a compiled song to the WAV file that export_wav() writes.
ExportResult render_wav(const std::shared_ptr<Song> &song, std::vector<uint8_t> &wav,
		Ym2612ChipType chip_type = Ym2612ChipType::Ym2612);

ExportResult export_wav(const std::string &in_path, const std::string &out_path,
		Ym2612ChipType chip_type = Ym2612ChipType::Ym2612);
ExportResult export_wav_text(const std::string &text, const std::string &base_path,
		const std::string &display_name, const std::string &out_path,
		Ym2612ChipType chip_type = Ym2612ChipType::Ym2612);
