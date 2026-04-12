#pragma once

#include <cstdint>
#include <memory>

#include <resampler/EmuStructs.h>

class YmfmYm2612Interface;
class YmfmYm2612Chip;

class YmfmYm2612Device
{
public:
	explicit YmfmYm2612Device(uint32_t clock);
	~YmfmYm2612Device();

	uint32_t sample_rate() const;
	void reset();
	void write(uint8_t offset, uint8_t data);
	void render(DEV_SMPL **outputs, uint32_t samples);
	void set_mute_mask(uint32_t mask);

	static void stream_update(void *info, UINT32 samples, DEV_SMPL **outputs);

private:
	uint32_t clock;
	uint32_t mute_mask;
	uint8_t port0_address;
	uint8_t key_state[8]; // operator key-on state per channel slot (indexed by 0x28 ch field)
	std::unique_ptr<YmfmYm2612Interface> interface;
	std::unique_ptr<YmfmYm2612Chip> chip;
};
