#pragma once

#include <cstdint>
#include <memory>

#include <resampler/stdtype.h>
#include <resampler/snddef.h>

class MameSn76496Device
{
public:
	MameSn76496Device(uint32_t clock, uint8_t lfsr_w = 0x10, uint16_t lfsr_t = 0x09);
	~MameSn76496Device();

	uint32_t sample_rate() const;
	void reset();
	void write(uint8_t data);
	void render(DEV_SMPL **outputs, uint32_t samples);
	void set_mute_mask(uint32_t mask);

	static void stream_update(void *info, UINT32 samples, DEV_SMPL **outputs);

private:
	uint32_t rate;
	void *chip_ptr;
};
