#include "preview_volume.h"

#include <cmath>
#include <cstdint>
#include <iostream>

int main()
{
	static_assert(preview_volume::gm2_velocity_tl.size() == 128);
	static_assert(sizeof(preview_volume::gm2_velocity_tl) == 128);

	for (int velocity = 0; velocity < 128; ++velocity)
	{
		const int regenerated = velocity == 0
			? 127
			: static_cast<int>(std::lround(40.0 * std::log10(127.0 / velocity) / 0.75));
		if (preview_volume::gm2_velocity_tl[velocity] != regenerated)
		{
			std::cerr << "GM2 table mismatch at velocity " << velocity
				<< ": expected " << regenerated << ", got "
				<< static_cast<int>(preview_volume::gm2_velocity_tl[velocity]) << '\n';
			return 1;
		}
	}

	struct AttenuationCase
	{
		uint8_t base;
		uint8_t attenuation;
		uint8_t expected;
	};
	constexpr AttenuationCase cases[] = {
		{0, 0, 0},
		{25, 42, 67},
		{100, 27, 127},
		{100, 28, 127},
		{127, 127, 127},
	};
	for (const auto &test : cases)
	{
		const uint8_t actual = preview_volume::add_tl_attenuation(test.base, test.attenuation);
		if (actual != test.expected)
		{
			std::cerr << "TL attenuation mismatch: expected " << static_cast<int>(test.expected)
				<< ", got " << static_cast<int>(actual) << '\n';
			return 1;
		}
	}

	return 0;
}
