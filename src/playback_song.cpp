// Layout, little-endian, varint being unsigned LEB128:
//
//     u32      size of the serialize_song() data, then that data
//     u32      file name count, then each name as a u32 length and its bytes
//     then for each event, in the order serialize_song() writes them:
//         varint   0 for an event without a location, else its file name
//                  index + 1, followed by:
//         varint   line
//         varint   column
//
// The locations become the events' InputRefs, so an InputError thrown during
// playback carries the same text as for the compiled song.

#include "playback_song.h"

#include <map>
#include <stdexcept>
#include <string>

#include "input.h"
#include "song_blob.h"
#include "track.h"

namespace
{
	void put_u32(std::vector<uint8_t> &out, uint32_t value)
	{
		for (int i = 0; i < 4; ++i)
			out.push_back(static_cast<uint8_t>(value >> (i * 8)));
	}

	void put_varint(std::vector<uint8_t> &out, uint32_t value)
	{
		while (value >= 0x80)
		{
			out.push_back(static_cast<uint8_t>(value | 0x80));
			value >>= 7;
		}
		out.push_back(static_cast<uint8_t>(value));
	}

	class Reader
	{
	public:
		Reader(const uint8_t *data, size_t size)
				: pos(data), end(data + size)
		{
		}

		const uint8_t *bytes(size_t size)
		{
			if (size > static_cast<size_t>(end - pos))
				throw std::invalid_argument("song data is truncated");
			const uint8_t *p = pos;
			pos += size;
			return p;
		}

		uint32_t u32()
		{
			const uint8_t *p = bytes(4);
			return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
		}

		//! Reads an element count, each element using at least `min_size` bytes.
		uint32_t count(size_t min_size)
		{
			const uint32_t value = u32();
			if (value > static_cast<size_t>(end - pos) / min_size)
				throw std::invalid_argument("song data is truncated");
			return value;
		}

		uint32_t varint()
		{
			uint32_t value = 0;
			for (int shift = 0; shift < 35; shift += 7)
			{
				const uint8_t byte = *bytes(1);
				value |= static_cast<uint32_t>(byte & 0x7f) << shift;
				if (!(byte & 0x80))
					return value;
			}
			throw std::invalid_argument("invalid song data");
		}

		bool at_end() const
		{
			return pos == end;
		}

	private:
		const uint8_t *pos;
		const uint8_t *end;
	};
}

std::vector<uint8_t> pack_playback_song(const Song &song)
{
	const std::vector<uint8_t> blob = serialize_song(song);
	std::vector<uint8_t> out;
	out.reserve(blob.size() + 64);
	put_u32(out, static_cast<uint32_t>(blob.size()));
	out.insert(out.end(), blob.begin(), blob.end());

	std::map<std::string, uint32_t> file_index;
	std::vector<const std::string *> files;
	for (const auto &track : song.get_track_map())
	{
		for (const Event &event : track.second.get_events())
		{
			if (!event.reference)
				continue;
			const std::string &file = event.reference->get_filename();
			if (file_index.emplace(file, static_cast<uint32_t>(files.size())).second)
				files.push_back(&file);
		}
	}
	put_u32(out, static_cast<uint32_t>(files.size()));
	for (const std::string *file : files)
	{
		put_u32(out, static_cast<uint32_t>(file->size()));
		out.insert(out.end(), file->begin(), file->end());
	}

	for (const auto &track : song.get_track_map())
	{
		for (const Event &event : track.second.get_events())
		{
			if (!event.reference)
			{
				put_varint(out, 0);
				continue;
			}
			put_varint(out, file_index.at(event.reference->get_filename()) + 1);
			put_varint(out, event.reference->get_line());
			put_varint(out, event.reference->get_column());
		}
	}
	return out;
}

std::shared_ptr<Song> unpack_playback_song(const uint8_t *data, size_t size)
{
	Reader in(data, size);
	const uint32_t blob_size = in.u32();
	const uint8_t *blob = in.bytes(blob_size);
	auto song = std::make_shared<Song>();
	deserialize_song(blob, blob_size, *song);

	std::vector<std::string> files(in.count(4));
	for (std::string &file : files)
	{
		const uint32_t length = in.u32();
		const uint8_t *chars = in.bytes(length);
		file.assign(reinterpret_cast<const char *>(chars), length);
	}

	size_t event_count = 0;
	for (auto &track : song->get_track_map())
		event_count += track.second.get_events().size();

	// One block holds every InputRef; the events share ownership of it.
	auto refs = std::make_shared<std::vector<InputRef>>();
	refs->reserve(event_count);
	for (auto &track : song->get_track_map())
	{
		for (Event &event : track.second.get_events())
		{
			const uint32_t file = in.varint();
			if (!file)
				continue;
			if (file > files.size())
				throw std::invalid_argument("invalid song data");
			const uint32_t line = in.varint();
			const uint32_t column = in.varint();
			refs->emplace_back(files[file - 1], "", static_cast<int>(line), static_cast<int>(column));
			event.reference = std::shared_ptr<InputRef>(refs, &refs->back());
		}
	}
	if (!in.at_end())
		throw std::invalid_argument("unexpected data after song");
	return song;
}
