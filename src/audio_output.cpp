#include "audio_output.h"

#include <cstring>

#include <audio/AudioStream.h>

namespace
{
	const float kInvSampleScale = 1.0f / 8388608.0f;
}

AudioOutput::AudioOutput()
		: drv(nullptr), renderer(nullptr), running(false)
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

	if (Audio_Init() != AERR_OK)
		return false;

	UINT32 driver_count = Audio_GetDriverCount();
	UINT32 driver_id = 0xffffffffu;
	for (UINT32 i = 0; i < driver_count; ++i)
	{
		AUDDRV_INFO *info = nullptr;
		if (Audio_GetDriverInfo(i, &info) != AERR_OK || !info)
			continue;
		if (info->drvType == ADRVTYPE_OUT)
		{
			driver_id = i;
			break;
		}
	}
	if (driver_id == 0xffffffffu)
		return false;

	if (AudioDrv_Init(driver_id, &drv) != AERR_OK || !drv)
		return false;

	AUDIO_OPTS *opts = AudioDrv_GetOptions(drv);
	if (opts)
	{
		opts->sampleRate = sample_rate;
		opts->numChannels = 2;
		opts->numBitsPerSmpl = 16;
	}

	lpf.init(sample_rate);

	if (AudioDrv_SetCallback(drv, &AudioOutput::fill_buffer, this) != AERR_OK)
		return false;
	if (AudioDrv_Start(drv, 0) != AERR_OK)
		return false;

	running = true;
	return true;
}

void AudioOutput::stop()
{
	if (!running)
		return;
	AudioDrv_Stop(drv);
	AudioDrv_Deinit(&drv);
	Audio_Deinit();
	drv = nullptr;
	running = false;
}

bool AudioOutput::is_running() const
{
	return running;
}

uint32_t AudioOutput::fill_buffer(void *drvStruct, void *userParam, uint32_t bufSize, void *data)
{
	(void)drvStruct;
	if (!userParam)
		return 0;
	return static_cast<AudioOutput *>(userParam)->fill(bufSize, data);
}

uint32_t AudioOutput::fill(uint32_t bufSize, void *data)
{
	if (!renderer || !data || bufSize == 0)
		return 0;

	const uint32_t frame_size = 2 * sizeof(int16_t);
	uint32_t frames = bufSize / frame_size;
	if (scratch.size() < frames)
		scratch.resize(frames);
	std::memset(scratch.data(), 0, frames * sizeof(WAVE_32BS));
	int written = renderer->get_sample(scratch.data(), static_cast<int>(frames));
	if (written < 0)
		written = 0;

	int16_t *out = static_cast<int16_t *>(data);
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
		out[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
		out[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
	}

	return static_cast<uint32_t>(written) * frame_size;
}
