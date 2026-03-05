#include "rom_builder.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{
	constexpr size_t kHeaderChecksumOffset = 0x18e;
	constexpr size_t kChecksumStartOffset = 0x200;
	constexpr char kTemplateMagic[] = "CTRMROM0";
	constexpr size_t kTemplateMagicSize = sizeof(kTemplateMagic) - 1;
	constexpr size_t kTemplateMetaSize = kTemplateMagicSize + 16;
	constexpr char kConfigMagic[] = "CTRMCFG0";
	constexpr size_t kConfigMagicSize = sizeof(kConfigMagic) - 1;
	constexpr size_t kConfigMetaSize = kConfigMagicSize + 16;

	struct SlotLayout
	{
		uint32_t seq_offset = 0;
		uint32_t seq_size = 0;
		uint32_t pcm_offset = 0;
		uint32_t pcm_size = 0;
		bool from_marker = false;
	};

	struct RuntimeConfigOffsets
	{
		uint32_t bgm_min_offset = 0;
		uint32_t bgm_max_offset = 0;
		uint32_t se_min_offset = 0;
		uint32_t se_max_offset = 0;
	};

	struct RuntimeConfigValues
	{
		uint16_t bgm_min = 0;
		uint16_t bgm_max = 0;
		uint16_t se_min = 0;
		uint16_t se_max = 0;
	};

	uint32_t read_be32(const std::vector<uint8_t> &bytes, size_t offset)
	{
		return (static_cast<uint32_t>(bytes[offset]) << 24)
					 | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
					 | (static_cast<uint32_t>(bytes[offset + 2]) << 8)
					 | static_cast<uint32_t>(bytes[offset + 3]);
	}

	void write_be16(std::vector<uint8_t> &bytes, size_t offset, uint16_t value)
	{
		bytes[offset] = static_cast<uint8_t>((value >> 8) & 0xffu);
		bytes[offset + 1] = static_cast<uint8_t>(value & 0xffu);
	}

	std::string trim(const std::string &value)
	{
		size_t begin = 0;
		while (begin < value.size()
					 && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
			++begin;
		size_t end = value.size();
		while (end > begin
					 && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
			--end;
		return value.substr(begin, end - begin);
	}

	bool parse_u16_value(const std::string &value, uint16_t &out)
	{
		try
		{
			size_t pos = 0;
			unsigned long parsed = std::stoul(value, &pos, 0);
			if (pos != value.size() || parsed > 0xfffful)
				return false;
			out = static_cast<uint16_t>(parsed);
			return true;
		}
		catch (...)
		{
			return false;
		}
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

	bool find_runtime_config_offsets(const std::vector<uint8_t> &rom,
																	 RuntimeConfigOffsets &offsets)
	{
		if (rom.size() < kConfigMetaSize)
			return false;

		const auto *magic = reinterpret_cast<const uint8_t *>(kConfigMagic);
		for (size_t i = 0; i + kConfigMetaSize <= rom.size(); ++i)
		{
			if (std::memcmp(rom.data() + static_cast<std::ptrdiff_t>(i), magic, kConfigMagicSize) != 0)
				continue;

			RuntimeConfigOffsets candidate{};
			candidate.bgm_min_offset = read_be32(rom, i + kConfigMagicSize + 0);
			candidate.bgm_max_offset = read_be32(rom, i + kConfigMagicSize + 4);
			candidate.se_min_offset = read_be32(rom, i + kConfigMagicSize + 8);
			candidate.se_max_offset = read_be32(rom, i + kConfigMagicSize + 12);

			const auto valid_word = [&](uint32_t offset) -> bool
			{
				return static_cast<size_t>(offset) + 1 < rom.size();
			};
			if (!valid_word(candidate.bgm_min_offset) || !valid_word(candidate.bgm_max_offset)
					|| !valid_word(candidate.se_min_offset) || !valid_word(candidate.se_max_offset))
				continue;

			offsets = candidate;
			return true;
		}

		return false;
	}

	bool resolve_runtime_config_values(const std::string &asm_header,
																	 RuntimeConfigValues &values,
																	 std::string &error)
	{
		bool has_bgm_min = false;
		bool has_bgm_max = false;
		bool has_se_min = false;
		bool has_se_max = false;
		uint16_t bgm_min = 0;
		uint16_t bgm_max = 0;
		uint16_t se_min = 0;
		uint16_t se_max = 0;

		std::istringstream stream(asm_header);
		std::string line;
		while (std::getline(stream, line))
		{
			auto eq = line.find('=');
			if (eq == std::string::npos)
				continue;
			std::string key = trim(line.substr(0, eq));
			std::string raw_value = trim(line.substr(eq + 1));
			uint16_t parsed = 0;

			if (key == "BGM_MIN")
			{
				if (!parse_u16_value(raw_value, parsed))
				{
					error = "failed to parse BGM_MIN in generated mds header";
					return false;
				}
				bgm_min = parsed;
				has_bgm_min = true;
			}
			else if (key == "BGM_MAX")
			{
				if (!parse_u16_value(raw_value, parsed))
				{
					error = "failed to parse BGM_MAX in generated mds header";
					return false;
				}
				bgm_max = parsed;
				has_bgm_max = true;
			}
			else if (key == "SE_MIN")
			{
				if (!parse_u16_value(raw_value, parsed))
				{
					error = "failed to parse SE_MIN in generated mds header";
					return false;
				}
				se_min = parsed;
				has_se_min = true;
			}
			else if (key == "SE_MAX")
			{
				if (!parse_u16_value(raw_value, parsed))
				{
					error = "failed to parse SE_MAX in generated mds header";
					return false;
				}
				se_max = parsed;
				has_se_max = true;
			}
		}

		if (!has_bgm_max)
		{
			if (has_se_min && se_min > 0)
				bgm_max = static_cast<uint16_t>(se_min - 1);
			else if (has_se_max)
				bgm_max = se_max;
			else
				bgm_max = 0;
			has_bgm_max = true;
		}
		if (!has_bgm_min)
		{
			bgm_min = (bgm_max > 0) ? 1 : 0;
			has_bgm_min = true;
		}
		if (!has_se_min)
		{
			uint32_t next = static_cast<uint32_t>(bgm_max) + 1u;
			se_min = static_cast<uint16_t>(std::min<uint32_t>(next, 0xffffu));
			has_se_min = true;
		}
		if (!has_se_max)
		{
			se_max = bgm_max;
			has_se_max = true;
		}

		if (!has_bgm_min || !has_bgm_max || !has_se_min || !has_se_max)
		{
			error = "missing required BGM/SE constants in generated mds header";
			return false;
		}

		values.bgm_min = bgm_min;
		values.bgm_max = bgm_max;
		values.se_min = se_min;
		values.se_max = se_max;
		return true;
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

	RuntimeConfigOffsets config_offsets{};
	if (!find_runtime_config_offsets(rom, config_offsets))
	{
		return {false,
						"template marker CTRMCFG0 not found. "
						"Use a marker-enabled template ROM built from the updated MDSDRV."};
	}

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
						layout.from_marker,
						0,
						0,
						0,
						0};
	}

	RuntimeConfigValues runtime_values{};
	std::string config_error;
	if (!resolve_runtime_config_values(build.payload.asm_header, runtime_values, config_error))
	{
		return {false,
						config_error,
						0,
						0,
						layout.seq_offset,
						layout.pcm_offset,
						layout.seq_size,
						layout.pcm_size,
						layout.from_marker,
						0,
						0,
						0,
						0};
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
						layout.from_marker,
						runtime_values.bgm_min,
						runtime_values.bgm_max,
						runtime_values.se_min,
						runtime_values.se_max};
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
						layout.from_marker,
						runtime_values.bgm_min,
						runtime_values.bgm_max,
						runtime_values.se_min,
						runtime_values.se_max};
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

	write_be16(rom,
						 static_cast<size_t>(config_offsets.bgm_min_offset),
						 runtime_values.bgm_min);
	write_be16(rom,
						 static_cast<size_t>(config_offsets.bgm_max_offset),
						 runtime_values.bgm_max);
	write_be16(rom,
						 static_cast<size_t>(config_offsets.se_min_offset),
						 runtime_values.se_min);
	write_be16(rom,
						 static_cast<size_t>(config_offsets.se_max_offset),
						 runtime_values.se_max);

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
							layout.from_marker,
							runtime_values.bgm_min,
							runtime_values.bgm_max,
							runtime_values.se_min,
							runtime_values.se_max};
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
						layout.from_marker,
						runtime_values.bgm_min,
						runtime_values.bgm_max,
						runtime_values.se_min,
						runtime_values.se_max};
	}

	return {true,
					"",
					seq_size,
					pcm_size,
					layout.seq_offset,
					layout.pcm_offset,
					layout.seq_size,
					layout.pcm_size,
					layout.from_marker,
					runtime_values.bgm_min,
					runtime_values.bgm_max,
					runtime_values.se_min,
					runtime_values.se_max};
}
