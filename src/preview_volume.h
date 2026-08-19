#pragma once

#include <array>
#include <cstdint>

namespace preview_volume
{
	// GM2/DLS square law: round(40 * log10(127 / velocity) / 0.75),
	// with velocity zero mapped to the YM2612's maximum TL attenuation.
	inline constexpr std::array<uint8_t, 128> gm2_velocity_tl = {
		127, 112, 96, 87, 80, 75, 71, 67, 64, 61, 59, 57, 55, 53, 51, 49,
		48, 47, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 33,
		32, 31, 31, 30, 29, 29, 28, 27, 27, 26, 26, 25, 25, 24, 24, 23,
		23, 22, 22, 21, 21, 20, 20, 19, 19, 19, 18, 18, 17, 17, 17, 16,
		16, 16, 15, 15, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12, 11, 11,
		11, 10, 10, 10, 10, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7,
		6, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 3, 3,
		3, 3, 3, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 0, 0, 0,
	};

	constexpr uint8_t add_tl_attenuation(uint8_t base_tl, uint8_t attenuation)
	{
		const int adjusted = static_cast<int>(base_tl) + attenuation;
		return static_cast<uint8_t>(adjusted > 127 ? 127 : adjusted);
	}
}
