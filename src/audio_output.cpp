#define MINIAUDIO_IMPLEMENTATION
#include "audio_output.h"

#include <cstring>

#include <miniaudio.h>

namespace
{
	const float kInvSampleScale = 1.0f / 8388608.0f;
}

AudioOutput::AudioOutput()
		: device(nullptr), renderer(nullptr), running(false)
{
}

AudioOutput::~AudioOutput()
{
	stop();
}

bool AudioOutput::start(VgmAudioRenderer *renderer_in, uint32_t sample_rate)
{
	if (running)
		return true;

	renderer = renderer_in;
	if (!renderer)
		return false;

	device = new ma_device;

	ma_device_config config = ma_device_config_init(ma_device_type_playback);
	config.playback.format = ma_format_s16;
	config.playback.channels = 2;
	config.sampleRate = sample_rate;
	config.dataCallback = &AudioOutput::data_callback;
	config.pUserData = this;

	if (ma_device_init(nullptr, &config, device) != MA_SUCCESS)
	{
		delete device;
		device = nullptr;
		return false;
	}

	lpf.init(sample_rate);

	if (ma_device_start(device) != MA_SUCCESS)
	{
		ma_device_uninit(device);
		delete device;
		device = nullptr;
		return false;
	}

	running = true;
	return true;
}

void AudioOutput::stop()
{
	if (!running)
		return;
	ma_device_stop(device);
	ma_device_uninit(device);
	delete device;
	device = nullptr;
	running = false;
}

bool AudioOutput::is_running() const
{
	return running;
}

void AudioOutput::data_callback(ma_device *dev, void *output, const void *input, uint32_t frame_count)
{
	(void)input;
	auto *self = static_cast<AudioOutput *>(dev->pUserData);
	if (!self)
	{
		std::memset(output, 0, frame_count * 2 * sizeof(int16_t));
		return;
	}
	self->fill(static_cast<int16_t *>(output), frame_count);
}

void AudioOutput::fill(int16_t *output, uint32_t frames)
{
	if (!renderer || frames == 0)
	{
		std::memset(output, 0, frames * 2 * sizeof(int16_t));
		return;
	}

	if (scratch.size() < frames)
		scratch.resize(frames);
	std::memset(scratch.data(), 0, frames * sizeof(WAVE_32BS));
	int written = renderer->get_sample(scratch.data(), static_cast<int>(frames));
	if (written < 0)
		written = 0;

	for (int i = 0; i < written; ++i)
	{
		lpf.apply(scratch[i].L, scratch[i].R);
		float l = scratch[i].L * kInvSampleScale;
		float r = scratch[i].R * kInvSampleScale;
		if (l > 1.0f)
			l = 1.0f;
		if (l < -1.0f)
			l = -1.0f;
		if (r > 1.0f)
			r = 1.0f;
		if (r < -1.0f)
			r = -1.0f;
		output[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
		output[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
	}

	// Zero remaining frames if renderer produced fewer
	if (static_cast<uint32_t>(written) < frames)
		std::memset(&output[written * 2], 0, (frames - written) * 2 * sizeof(int16_t));
}
