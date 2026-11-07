#pragma once

#include <cstdint>

struct DcBlocker
{
	void reset()
	{
		x_left = 0.0f;
		x_right = 0.0f;
		y_left = 0.0f;
		y_right = 0.0f;
	}

	void process(float &left, float &right)
	{
		// The YM2612 ladder's DC offset must not reach modern output amps:
		// their signal-energy standby transitions turn it into audible pops.
		constexpr float pole = 0.9995f;
		const float next_left = left - x_left + pole * y_left;
		const float next_right = right - x_right + pole * y_right;
		x_left = left;
		x_right = right;
		y_left = next_left;
		y_right = next_right;
		left = next_left;
		right = next_right;
	}

	//! Scales a mixed chip sample by 1/2^23 and `gain`, removes DC and clamps
	//! the result to [-1, 1].
	void to_output(int32_t in_left, int32_t in_right, float &left, float &right, float gain = 1.0f)
	{
		constexpr float scale = 1.0f / 8388608.0f;
		left = in_left * scale * gain;
		right = in_right * scale * gain;
		process(left, right);
		if (left > 1.0f)
			left = 1.0f;
		if (left < -1.0f)
			left = -1.0f;
		if (right > 1.0f)
			right = 1.0f;
		if (right < -1.0f)
			right = -1.0f;
	}

	float x_left = 0.0f;
	float x_right = 0.0f;
	float y_left = 0.0f;
	float y_right = 0.0f;
};
