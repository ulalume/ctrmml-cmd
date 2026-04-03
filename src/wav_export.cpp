#include "wav_export.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include "lowpass_filter.h"
#include "mml_compile.h"
#include "vgm_audio_renderer.h"

namespace
{
	const unsigned int kWavBufferFrames = 2048;
	const unsigned int kWavSampleRate = 44100;
	const unsigned int kWavBitDepth = 16;
	const unsigned int kWavChannels = 2;
	const unsigned int kWavFadeSeconds = 8;
	const unsigned int kWavLoops = 2;
	const float kInvSampleScale = 1.0f / 8388608.0f;
	const char *kExtensibleGuidTrailer = "\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71";

	void pack_int16le(uint8_t *d, int16_t n)
	{
		d[0] = static_cast<uint8_t>(static_cast<uint16_t>(n));
		d[1] = static_cast<uint8_t>(static_cast<uint16_t>(n) >> 8);
	}

	void pack_uint16le(uint8_t *d, uint16_t n)
	{
		d[0] = static_cast<uint8_t>(n);
		d[1] = static_cast<uint8_t>(n >> 8);
	}

	void pack_uint32le(uint8_t *d, uint32_t n)
	{
		d[0] = static_cast<uint8_t>(n);
		d[1] = static_cast<uint8_t>(n >> 8);
		d[2] = static_cast<uint8_t>(n >> 16);
		d[3] = static_cast<uint8_t>(n >> 24);
	}

	bool write_wav_header(FILE *f, unsigned int total_frames)
	{
		unsigned int data_size = total_frames * (kWavBitDepth / 8) * kWavChannels;
		uint8_t tmp[4];
		if (fwrite("RIFF", 1, 4, f) != 4)
			return false;
		pack_uint32le(tmp, 4 + (8 + data_size) + (8 + 40));
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		if (fwrite("WAVE", 1, 4, f) != 4)
			return false;
		if (fwrite("fmt ", 1, 4, f) != 4)
			return false;

		pack_uint32le(tmp, 40);
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		pack_uint16le(tmp, 0xFFFE);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint16le(tmp, kWavChannels);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint32le(tmp, kWavSampleRate);
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		pack_uint32le(tmp, kWavSampleRate * kWavChannels * (kWavBitDepth / 8));
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		pack_uint16le(tmp, kWavChannels * (kWavBitDepth / 8));
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint16le(tmp, kWavBitDepth);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint16le(tmp, 22);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint16le(tmp, kWavBitDepth);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint32le(tmp, 3);
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		pack_uint16le(tmp, 1);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		if (fwrite(kExtensibleGuidTrailer, 1, 14, f) != 14)
			return false;

		if (fwrite("data", 1, 4, f) != 4)
			return false;

		pack_uint32le(tmp, data_size);
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		return true;
	}

	bool export_wav_song(const std::shared_ptr<Song> &song, const std::string &out_path)
	{
		if (!song)
			return false;

		VgmAudioRenderer renderer(song, 0, false);
		renderer.setup_stream(kWavSampleRate);

		LowPassFilter lpf;
		lpf.init(kWavSampleRate);

		// Pass 1: render to memory to determine total length and handle loops
		const unsigned int fade_frames = kWavSampleRate * kWavFadeSeconds;
		const unsigned int max_frames = kWavSampleRate * 600; // 10 minute safety limit
		std::vector<int16_t> pcm_data;
		pcm_data.reserve(kWavSampleRate * 60 * kWavChannels); // pre-allocate ~1 min

		std::vector<WAVE_32BS> scratch(kWavBufferFrames);
		bool has_loop = false;
		unsigned int frames_rendered = 0;

		while (!renderer.is_finished() && frames_rendered < max_frames)
		{
			unsigned int chunk = kWavBufferFrames;
			std::memset(scratch.data(), 0, chunk * sizeof(WAVE_32BS));
			renderer.get_sample(scratch.data(), static_cast<int>(chunk));

			for (unsigned int i = 0; i < chunk; ++i)
			{
				lpf.apply(scratch[i].L, scratch[i].R);
				float l = scratch[i].L * kInvSampleScale;
				float r = scratch[i].R * kInvSampleScale;
				if (l > 1.0f) l = 1.0f;
				if (l < -1.0f) l = -1.0f;
				if (r > 1.0f) r = 1.0f;
				if (r < -1.0f) r = -1.0f;
				pcm_data.push_back(static_cast<int16_t>(l * 32767.0f));
				pcm_data.push_back(static_cast<int16_t>(r * 32767.0f));
			}

			frames_rendered += chunk;

			if (renderer.get_loop_count() >= static_cast<int>(kWavLoops))
			{
				has_loop = true;
				break;
			}
		}

		// Apply fade-out if the song loops
		unsigned int total_frames = frames_rendered;
		if (has_loop)
		{
			// Render additional frames for fade-out
			unsigned int fade_rendered = 0;
			while (fade_rendered < fade_frames && !renderer.is_finished())
			{
				unsigned int chunk = std::min(kWavBufferFrames, fade_frames - fade_rendered);
				std::memset(scratch.data(), 0, chunk * sizeof(WAVE_32BS));
				renderer.get_sample(scratch.data(), static_cast<int>(chunk));

				for (unsigned int i = 0; i < chunk; ++i)
				{
					lpf.apply(scratch[i].L, scratch[i].R);
					float fade = 1.0f - static_cast<float>(fade_rendered + i) / static_cast<float>(fade_frames);
					float l = scratch[i].L * kInvSampleScale * fade;
					float r = scratch[i].R * kInvSampleScale * fade;
					if (l > 1.0f) l = 1.0f;
					if (l < -1.0f) l = -1.0f;
					if (r > 1.0f) r = 1.0f;
					if (r < -1.0f) r = -1.0f;
					pcm_data.push_back(static_cast<int16_t>(l * 32767.0f));
					pcm_data.push_back(static_cast<int16_t>(r * 32767.0f));
				}
				fade_rendered += chunk;
			}
			total_frames += fade_rendered;
		}

		// Write WAV file
		FILE *out = fopen(out_path.c_str(), "wb");
		if (!out)
		{
			std::cerr << "unable to open output file" << std::endl;
			return false;
		}

		if (!write_wav_header(out, total_frames))
		{
			std::cerr << "failed to write wav header" << std::endl;
			fclose(out);
			return false;
		}

		// Write PCM data in chunks (little-endian conversion)
		{
			const unsigned int chunk_samples = kWavBufferFrames * kWavChannels;
			uint8_t chunk_buf[chunk_samples * (kWavBitDepth / 8)];
			unsigned int remaining = total_frames * kWavChannels;
			unsigned int pos = 0;
			bool write_ok = true;

			while (remaining > 0 && write_ok)
			{
				unsigned int n = std::min(remaining, chunk_samples);
				for (unsigned int i = 0; i < n; ++i)
					pack_int16le(&chunk_buf[i * 2], pcm_data[pos + i]);
				write_ok = (fwrite(chunk_buf, 2, n, out) == n);
				pos += n;
				remaining -= n;
			}

			if (!write_ok)
			{
				std::cerr << "failed to write wav data" << std::endl;
				fclose(out);
				return false;
			}
		}

		fclose(out);
		return true;
	}
}

bool export_wav(const std::string &in_path, const std::string &out_path)
{
	auto compile = compile_mml_file(in_path);
	if (!compile.song)
	{
		std::cerr << compile.error << std::endl;
		return false;
	}
	return export_wav_song(compile.song, out_path);
}

bool export_wav_text(const std::string &text, const std::string &base_path, const std::string &display_name, const std::string &out_path)
{
	auto compile = compile_mml_text(text, base_path, display_name);
	if (!compile.song)
	{
		std::cerr << compile.error << std::endl;
		return false;
	}
	return export_wav_song(compile.song, out_path);
}
