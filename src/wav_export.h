#pragma once

#include <string>

#include "export_result.h"

ExportResult export_wav(const std::string& in_path, const std::string& out_path);
ExportResult export_wav_text(const std::string& text, const std::string& base_path, const std::string& display_name, const std::string& out_path);
