#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "song.h"

//! A compiled song in the form the player module loads: serialize_song()
//! followed by the source location of every event, which serialize_song()
//! leaves out, so that playback errors keep their line and column.
std::vector<uint8_t> pack_playback_song(const Song &song);

//! Reads what pack_playback_song() wrote.
//! \exception std::invalid_argument if the data is not a packed song.
std::shared_ptr<Song> unpack_playback_song(const uint8_t *data, size_t size);
