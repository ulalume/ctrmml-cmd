#pragma once

#include <cstdint>
#include <vector>

#include <emu/EmuStructs.h>

#include "lowpass_filter.h"
#include "vgm_audio_renderer.h"

class AudioOutput
{
public:
	AudioOutput();
	~AudioOutput();

	bool start(VgmAudioRenderer *renderer, uint32_t sample_rate);
	void stop();
	bool is_running() const;

private:
	static uint32_t fill_buffer(void *drvStruct, void *userParam, uint32_t bufSize, void *data);
	uint32_t fill(uint32_t bufSize, void *data);

	void *drv;
	VgmAudioRenderer *renderer;
	bool running;
	LowPassFilter lpf;
	std::vector<WAVE_32BS> scratch;
};
