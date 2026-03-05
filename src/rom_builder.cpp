#include "rom_builder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace
{
	constexpr size_t kHeaderChecksumOffset = 0x18e;
	constexpr size_t kChecksumStartOffset = 0x200;
	constexpr char kTemplateMagic[] = "CTRMROM0";
	constexpr size_t kTemplateMagicSize = sizeof(kTemplateMagic) - 1;
	constexpr size_t kTemplateMetaSize = kTemplateMagicSize + 16;

	struct SlotLayout
	{
		uint32_t seq_offset = 0;
		uint32_t seq_size = 0;
		uint32_t pcm_offset = 0;
		uint32_t pcm_size = 0;
		bool from_marker = false;
	};

	uint32_t read_be32(const std::vector<uint8_t> &bytes, size_t offset)
	{
		return (static_cast<uint32_t>(bytes[offset]) << 24)
					 | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
					 | (static_cast<uint32_t>(bytes[offset + 2]) << 8)
					 | static_cast<uint32_t>(bytes[offset + 3]);
	}

	bool write_binary_file(const std::string &path, const std::vector<uint8_t> &bytes, std::string &error)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			error = "failed to open output rom: " + path;
			return false;
		}
		output.write(reinterpret_cast<const char *>(bytes.data()),
								 static_cast<std::streamsize>(bytes.size()));
		if (!output)
		{
			error = "failed to write output rom: " + path;
			return false;
		}
		return true;
	}

	bool slot_is_valid(size_t rom_size, size_t offset, size_t slot_size)
	{
		return offset <= rom_size && slot_size <= (rom_size - offset);
	}

	bool find_template_layout(const std::vector<uint8_t> &rom, SlotLayout &layout)
	{
		if (rom.size() < kTemplateMetaSize)
			return false;

		const auto *magic = reinterpret_cast<const uint8_t *>(kTemplateMagic);
		for (size_t i = 0; i + kTemplateMetaSize <= rom.size(); ++i)
		{
			if (std::memcmp(rom.data() + static_cast<std::ptrdiff_t>(i), magic, kTemplateMagicSize) != 0)
				continue;

			SlotLayout candidate{};
			candidate.seq_offset = read_be32(rom, i + kTemplateMagicSize + 0);
			candidate.seq_size = read_be32(rom, i + kTemplateMagicSize + 4);
			candidate.pcm_offset = read_be32(rom, i + kTemplateMagicSize + 8);
			candidate.pcm_size = read_be32(rom, i + kTemplateMagicSize + 12);
			candidate.from_marker = true;

			if (!slot_is_valid(rom.size(),
											 static_cast<size_t>(candidate.seq_offset),
											 static_cast<size_t>(candidate.seq_size)))
				continue;
			if (!slot_is_valid(rom.size(),
											 static_cast<size_t>(candidate.pcm_offset),
											 static_cast<size_t>(candidate.pcm_size)))
				continue;

			layout = candidate;
			return true;
		}
		return false;
	}

	bool resolve_slot_layout(const std::vector<uint8_t> &rom,
													 SlotLayout &layout,
													 std::string &error)
	{
		SlotLayout from_marker{};
		if (!find_template_layout(rom, from_marker))
		{
			error = "template marker CTRMROM0 not found. "
							"Use a marker-enabled template ROM built from the updated MDSDRV.";
			return false;
		}

		layout = from_marker;
		layout.from_marker = true;

		if (layout.seq_size == 0 || layout.pcm_size == 0)
		{
			error = "seq/pcm slot sizes must be greater than zero";
			return false;
		}
		if (!slot_is_valid(rom.size(),
											 static_cast<size_t>(layout.seq_offset),
											 static_cast<size_t>(layout.seq_size)))
		{
			error = "mdsseq slot is out of ROM range (offset="
							+ std::to_string(layout.seq_offset)
							+ ", size=" + std::to_string(layout.seq_size) + ")";
			return false;
		}
		if (!slot_is_valid(rom.size(),
											 static_cast<size_t>(layout.pcm_offset),
											 static_cast<size_t>(layout.pcm_size)))
		{
			error = "mdspcm slot is out of ROM range (offset="
							+ std::to_string(layout.pcm_offset)
							+ ", size=" + std::to_string(layout.pcm_size) + ")";
			return false;
		}
		return true;
	}

	void write_slot(std::vector<uint8_t> &rom,
									size_t offset,
									size_t slot_size,
									uint8_t fill_value,
									const std::vector<uint8_t> &payload)
	{
		std::fill(rom.begin() + static_cast<std::ptrdiff_t>(offset),
							rom.begin() + static_cast<std::ptrdiff_t>(offset + slot_size),
							fill_value);
		std::copy(payload.begin(),
							payload.end(),
							rom.begin() + static_cast<std::ptrdiff_t>(offset));
	}

	uint16_t calculate_md_checksum(const std::vector<uint8_t> &rom)
	{
		if (rom.size() <= kChecksumStartOffset)
			return 0;

		uint32_t sum = 0;
		size_t i = kChecksumStartOffset;
		for (; i + 1 < rom.size(); i += 2)
		{
			sum += (static_cast<uint16_t>(rom[i]) << 8) | static_cast<uint16_t>(rom[i + 1]);
		}
		if (i < rom.size())
			sum += static_cast<uint16_t>(rom[i]) << 8;
		return static_cast<uint16_t>(sum & 0xffffu);
	}
}

RomBuildResult run_rom_build(const RomBuildOptions &options)
{
	if (options.template_rom_bytes.empty())
		return {false, "embedded template ROM is missing"};
	if (options.output_rom_path.empty())
		return {false, "output rom path is required"};

	std::vector<uint8_t> rom = options.template_rom_bytes;

	SlotLayout layout{};
	std::string layout_error;
	if (!resolve_slot_layout(rom, layout, layout_error))
		return {false, layout_error};

	auto build = build_mdslink_payload(options.mdslink);
	if (!build.ok)
	{
		return {false,
						build.error,
						0,
						0,
						layout.seq_offset,
						layout.pcm_offset,
						layout.seq_size,
						layout.pcm_size,
						layout.from_marker};
	}

	const auto seq_size = build.payload.seq_data.size();
	const auto pcm_size = build.payload.pcm_data.size();
	if (seq_size > static_cast<size_t>(layout.seq_size))
	{
		return {false,
						"mdsseq payload exceeds slot size (" + std::to_string(seq_size) + " > "
								+ std::to_string(layout.seq_size) + ")",
						seq_size,
						pcm_size,
						layout.seq_offset,
						layout.pcm_offset,
						layout.seq_size,
						layout.pcm_size,
						layout.from_marker};
	}
	if (pcm_size > static_cast<size_t>(layout.pcm_size))
	{
		return {false,
						"mdspcm payload exceeds slot size (" + std::to_string(pcm_size) + " > "
								+ std::to_string(layout.pcm_size) + ")",
						seq_size,
						pcm_size,
						layout.seq_offset,
						layout.pcm_offset,
						layout.seq_size,
						layout.pcm_size,
						layout.from_marker};
	}

	write_slot(rom,
						 static_cast<size_t>(layout.seq_offset),
						 static_cast<size_t>(layout.seq_size),
						 options.fill_value,
						 build.payload.seq_data);
	write_slot(rom,
						 static_cast<size_t>(layout.pcm_offset),
						 static_cast<size_t>(layout.pcm_size),
						 options.fill_value,
						 build.payload.pcm_data);

	if (options.update_checksum)
	{
		if (rom.size() < (kHeaderChecksumOffset + 2))
		{
			return {false,
							"template rom is too small to contain checksum field",
							seq_size,
							pcm_size,
							layout.seq_offset,
							layout.pcm_offset,
							layout.seq_size,
							layout.pcm_size,
							layout.from_marker};
		}
		uint16_t checksum = calculate_md_checksum(rom);
		rom[kHeaderChecksumOffset] = static_cast<uint8_t>((checksum >> 8) & 0xffu);
		rom[kHeaderChecksumOffset + 1] = static_cast<uint8_t>(checksum & 0xffu);
	}

	std::string io_error;
	if (!write_binary_file(options.output_rom_path, rom, io_error))
	{
		return {false,
						io_error,
						seq_size,
						pcm_size,
						layout.seq_offset,
						layout.pcm_offset,
						layout.seq_size,
						layout.pcm_size,
						layout.from_marker};
	}

	return {true,
					"",
					seq_size,
					pcm_size,
					layout.seq_offset,
					layout.pcm_offset,
					layout.seq_size,
					layout.pcm_size,
					layout.from_marker};
}
