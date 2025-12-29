#pragma once

#include <string>

bool export_vgm(const std::string& in_path, const std::string& out_path);
bool export_vgm_text(const std::string& text, const std::string& base_path, const std::string& display_name, const std::string& out_path);
