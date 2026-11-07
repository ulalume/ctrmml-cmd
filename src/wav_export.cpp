#include "wav_export.h"

#include <cstring>
#include <vector>

#include "binary_file.h"
#include "dc_blocker.h"
#include "input.h"
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
	const char *kExtensibleGuidTrailer = "\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71";

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

	// RIFF header with a WAVE_FORMAT_EXTENSIBLE fmt chunk, then the data
	// chunk header.
	constexpr size_t kWavHeaderSize = 68;

	void pack_wav_header(uint8_t *d, unsigned int total_frames)
	{
		const unsigned int data_size = total_frames * (kWavBitDepth / 8) * kWavChannels;
		std::memcpy(d + 0, "RIFF", 4);
		pack_uint32le(d + 4, 4 + (8 + data_size) + (8 + 40));
		std::memcpy(d + 8, "WAVE", 4);
		std::memcpy(d + 12, "fmt ", 4);
		pack_uint32le(d + 16, 40);
		pack_uint16le(d + 20, 0xFFFE);
		pack_uint16le(d + 22, kWavChannels);
		pack_uint32le(d + 24, kWavSampleRate);
		pack_uint32le(d + 28, kWavSampleRate * kWavChannels * (kWavBitDepth / 8));
		pack_uint16le(d + 32, kWavChannels * (kWavBitDepth / 8));
		pack_uint16le(d + 34, kWavBitDepth);
		pack_uint16le(d + 36, 22);
		pack_uint16le(d + 38, kWavBitDepth);
		pack_uint32le(d + 40, 3);
		pack_uint16le(d + 44, 1);
		std::memcpy(d + 46, kExtensibleGuidTrailer, 14);
		std::memcpy(d + 60, "data", 4);
		pack_uint32le(d + 64, data_size);
	}

	// Appends one stereo frame as 16-bit little-endian samples.
	void append_frame(std::vector<uint8_t> &out, float left, float right)
	{
		for (float value : {left, right})
		{
			const uint16_t sample = static_cast<uint16_t>(static_cast<int16_t>(value * 32767.0f));
			out.push_back(static_cast<uint8_t>(sample));
			out.push_back(static_cast<uint8_t>(sample >> 8));
		}
	}

	ExportResult render_wav_song(const std::shared_ptr<Song> &song, std::vector<uint8_t> &wav,
			Ym2612ChipType chip_type)
	{
		if (!song)
			return {false, "no compiled song"};

		VgmAudioRenderer renderer(song, 0, false);
		renderer.set_ym2612_chip_type(chip_type);
		renderer.setup_stream(kWavSampleRate);

		DcBlocker dc_blocker;
		dc_blocker.reset();

		// Pass 1: render to memory to determine total length and handle loops
		const unsigned int fade_frames = kWavSampleRate * kWavFadeSeconds;
		const unsigned int max_frames = kWavSampleRate * 600; // 10 minute safety limit
		wav.assign(kWavHeaderSize, 0);
		wav.reserve(kWavHeaderSize + kWavSampleRate * 60 * kWavChannels * (kWavBitDepth / 8)); // ~1 min

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
				float l, r;
				dc_blocker.to_output(scratch[i].L, scratch[i].R, l, r);
				append_frame(wav, l, r);
			}

			frames_rendered += chunk;

			if (renderer.get_loop_count() >= static_cast<int>(kWavLoops))
			{
				has_loop = true;
				break;
			}
		}

		// get_sample() intentionally catches playback exceptions for realtime
		// playback and reports them through last_error(). Export must turn that
		// normal early stop into a failure before it creates a partial WAV.
		if (!renderer.last_error().empty())
			return {false, renderer.last_error()};

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
					float fade = 1.0f - static_cast<float>(fade_rendered + i) / static_cast<float>(fade_frames);
					float l, r;
					dc_blocker.to_output(scratch[i].L, scratch[i].R, l, r, fade);
					append_frame(wav, l, r);
				}
				fade_rendered += chunk;
			}
			total_frames += fade_rendered;
		}

		if (!renderer.last_error().empty())
			return {false, renderer.last_error()};

		pack_wav_header(wav.data(), total_frames);
		return {true, {}};
	}

	ExportResult safe_export_wav_song(const std::shared_ptr<Song> &song, const std::string &out_path,
			Ym2612ChipType chip_type)
	{
		std::vector<uint8_t> wav;
		ExportResult result = render_wav(song, wav, chip_type);
		if (!result.ok)
			return result;
		std::string error;
		if (!write_binary_file(out_path, wav, error))
			return {false, error};
		return {true, {}};
	}
}

ExportResult render_wav(const std::shared_ptr<Song> &song, std::vector<uint8_t> &wav,
		Ym2612ChipType chip_type)
{
	try
	{
		return render_wav_song(song, wav, chip_type);
	}
	catch (InputError &e)
	{
		return {false, e.what()};
	}
	catch (std::exception &e)
	{
		return {false, e.what()};
	}
}

ExportResult export_wav(const std::string &in_path, const std::string &out_path,
		Ym2612ChipType chip_type)
{
	auto compile = compile_mml_file(in_path);
	if (!compile.song)
		return {false, compile.error};
	return safe_export_wav_song(compile.song, out_path, chip_type);
}

ExportResult export_wav_text(const std::string &text, const std::string &base_path,
		const std::string &display_name, const std::string &out_path,
		Ym2612ChipType chip_type)
{
	auto compile = compile_mml_text(text, base_path, display_name);
	if (!compile.song)
		return {false, compile.error};
	return safe_export_wav_song(compile.song, out_path, chip_type);
}
