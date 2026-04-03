#pragma once

#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// First-order IIR low-pass filter (RC filter), matching BlastEm's
// analog output stage emulation.
// Default cutoff 3390 Hz, same as BlastEm's DEFAULT_LOWPASS_CUTOFF.
struct LowPassFilter
{
	int32_t last_l = 0;
	int32_t last_r = 0;
	int32_t alpha = 0; // 0.16 fixed-point coefficient
	bool enabled = false;
	uint32_t current_sample_rate = 0;

	void init(uint32_t sample_rate, double cutoff_hz = 3390.0)
	{
		current_sample_rate = sample_rate;
		set_cutoff(cutoff_hz);
		last_l = 0;
		last_r = 0;
		enabled = true;
	}

	void set_cutoff(double cutoff_hz)
	{
		if (current_sample_rate == 0 || cutoff_hz <= 0.0)
			return;
		double rc = 1.0 / (2.0 * M_PI * cutoff_hz);
		double dt = 1.0 / current_sample_rate;
		alpha = static_cast<int32_t>(65536.0 * dt / (dt + rc));
	}

	void apply(int32_t &l, int32_t &r)
	{
		if (!enabled)
			return;
		int64_t tl = static_cast<int64_t>(l) * alpha
		           + static_cast<int64_t>(last_l) * (65536 - alpha);
		int64_t tr = static_cast<int64_t>(r) * alpha
		           + static_cast<int64_t>(last_r) * (65536 - alpha);
		last_l = static_cast<int32_t>(tl >> 16);
		last_r = static_cast<int32_t>(tr >> 16);
		l = last_l;
		r = last_r;
	}
};
