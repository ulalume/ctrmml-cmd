#pragma once

#include <cstdint>
#include <vector>

#include "dc_blocker.h"
#include "lowpass_filter.h"
#include "vgm_audio_renderer.h"

struct ma_device;

class AudioOutput
{
public:
	AudioOutput();
	~AudioOutput();

	bool start(VgmAudioRenderer *renderer, uint32_t sample_rate);
	void stop();
	bool is_running() const;

private:
	static void data_callback(ma_device *device, void *output, const void *input, uint32_t frame_count);
	void fill(int16_t *output, uint32_t frames);

	ma_device *device;
	VgmAudioRenderer *renderer;
	bool running;
	LowPassFilter lpf;
	DcBlocker dc_blocker;
	std::vector<WAVE_32BS> scratch;
};
