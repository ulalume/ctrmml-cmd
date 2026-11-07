#pragma once

#include <cstdint>
#include <string>
#include <vector>

//! Writes `bytes` to `path`, replacing the file. On failure removes what was
//! written, sets `error` and returns false.
bool write_binary_file(const std::string &path, const std::vector<uint8_t> &bytes, std::string &error);
