#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "dc_blocker.h"
#include "lowpass_filter.h"
#include "preview_synth.h"
#include "vgm_audio_renderer.h"
#include "ymfm_ym2612_device.h"

namespace
{
	constexpr uint32_t kYm2612Clock = 7670454;
	constexpr uint32_t kSampleRate = 44100;
	constexpr uint32_t kPsgChipRate = 223723;
	constexpr uint32_t kSn76496Clock = 3579545;

	bool check(bool condition, const char *message)
	{
		if (!condition)
			std::cerr << "FAIL: " << message << '\n';
		return condition;
	}

	void write_register(YmfmYm2612Device &device, uint8_t port, uint8_t reg, uint8_t data)
	{
		const uint8_t offset = static_cast<uint8_t>(port << 1);
		device.write(offset, reg);
		device.write(static_cast<uint8_t>(offset + 1), data);
	}

	void configure_loud_note(YmfmYm2612Device &device, int channels)
	{
		for (int channel = 0; channel < channels; ++channel)
		{
			const uint8_t port = channel < 3 ? 0 : 1;
			const uint8_t index = static_cast<uint8_t>(channel % 3);
			write_register(device, port, static_cast<uint8_t>(0xb0 + index), 0x07);
			write_register(device, port, static_cast<uint8_t>(0xb4 + index), 0xc0);

			for (uint8_t slot = 0; slot < 4; ++slot)
			{
				const uint8_t offset = static_cast<uint8_t>(index + slot * 4);
				write_register(device, port, static_cast<uint8_t>(0x30 + offset), 0x01);
				write_register(device, port, static_cast<uint8_t>(0x40 + offset), 0x00);
				write_register(device, port, static_cast<uint8_t>(0x50 + offset), 0x1f);
				write_register(device, port, static_cast<uint8_t>(0x60 + offset), 0x00);
				write_register(device, port, static_cast<uint8_t>(0x70 + offset), 0x00);
				write_register(device, port, static_cast<uint8_t>(0x80 + offset), 0x0f);
			}

			write_register(device, port, static_cast<uint8_t>(0xa4 + index), 0x2a);
			write_register(device, port, static_cast<uint8_t>(0xa0 + index), 0x84);
			const uint8_t key_channel = static_cast<uint8_t>(channel < 3 ? channel : channel + 1);
			write_register(device, 0, 0x28, static_cast<uint8_t>(0xf0 | key_channel));
		}
	}

	std::vector<int32_t> render_left(YmfmYm2612Device &device, uint32_t frames)
	{
		std::vector<int32_t> left(frames);
		std::vector<int32_t> right(frames);
		DEV_SMPL *outputs[] = {left.data(), right.data()};
		device.render(outputs, frames);
		return left;
	}

	float normalized_peak(const std::vector<int32_t> &samples)
	{
		int32_t peak = 0;
		for (int32_t sample : samples)
			peak = std::max(peak, static_cast<int32_t>(std::abs(static_cast<int64_t>(sample))));
		return static_cast<float>(peak) / 32768.0f;
	}

	bool test_gain_staging()
	{
		YmfmYm2612Device six_channels(kYm2612Clock);
		configure_loud_note(six_channels, 6);
		const float six_peak = normalized_peak(render_left(six_channels, kSampleRate));

		YmfmYm2612Device three_channels(kYm2612Clock);
		configure_loud_note(three_channels, 3);
		const float three_peak = normalized_peak(render_left(three_channels, kSampleRate));

		std::cout << "FM peaks: 6ch=" << six_peak << ", 3ch=" << three_peak << '\n';
		bool ok = true;
		ok &= check(six_peak > 0.90f && six_peak <= 1.01f,
				"six-channel FM peak should be approximately full scale");
		ok &= check(three_peak > 0.10f && three_peak < 1.0f,
				"three-channel FM peak should remain below full scale");
		return ok;
	}

	bool test_chip_types()
	{
		bool ok = true;
		YmfmYm2612Device ladder(kYm2612Clock, Ym2612ChipType::Ym2612);
		auto ladder_idle = render_left(ladder, 16);
		for (int32_t sample : ladder_idle)
			ok &= check(sample == 504, "YM2612 raw idle output should be exactly 504");

		YmfmYm2612Device clean(kYm2612Clock, Ym2612ChipType::Ym3438);
		auto clean_idle = render_left(clean, 16);
		for (int32_t sample : clean_idle)
			ok &= check(sample == 0, "YM3438 raw idle output should be exactly zero");

		configure_loud_note(ladder, 1);
		configure_loud_note(clean, 1);
		ok &= check(normalized_peak(render_left(ladder, kSampleRate / 10)) > 0.01f,
				"YM2612 mode should render a note");
		ok &= check(normalized_peak(render_left(clean, kSampleRate / 10)) > 0.01f,
				"YM3438 mode should render a note");
		return ok;
	}

	bool test_dc_blocker()
	{
		constexpr float raw_dc = 504.0f / 32768.0f;
		DcBlocker blocker;
		blocker.reset();

		double settled_sum = 0.0;
		for (uint32_t frame = 0; frame < kSampleRate; ++frame)
		{
			float left = raw_dc;
			float right = raw_dc;
			blocker.process(left, right);
			if (frame >= kSampleRate / 2)
				settled_sum += left;
		}
		const double settled_mean = settled_sum / static_cast<double>(kSampleRate / 2);
		std::cout << "DC means: raw=" << raw_dc << ", settled=" << settled_mean << '\n';
		return check(raw_dc > 0.015f, "raw YM2612 DC should be present") &&
				check(std::abs(settled_mean) < 1.0e-6, "DC blocker output should converge to zero mean");
	}

	// Peak of a sine at `freq` after `filter`, relative to its input peak.
	double filtered_gain(LowPassFilter &filter, uint32_t rate, double freq)
	{
		constexpr double amplitude = 1000000.0;
		const uint32_t frames = rate / 50;
		int32_t peak = 0;
		for (uint32_t i = 0; i < frames; ++i)
		{
			const double phase = 2.0 * M_PI * freq * i / rate;
			int32_t l = static_cast<int32_t>(amplitude * std::sin(phase));
			int32_t r = l;
			filter.apply(l, r);
			if (i >= frames / 2)
				peak = std::max(peak, static_cast<int32_t>(std::abs(l)));
		}
		return peak / amplitude;
	}

	bool test_lowpass_response()
	{
		LowPassFilter filter;
		filter.init(kPsgChipRate);
		const double at_cutoff = filtered_gain(filter, kPsgChipRate, 3390.0);
		filter.init(kPsgChipRate);
		const double at_dc = filtered_gain(filter, kPsgChipRate, 100.0);
		const double cutoff_db = 20.0 * std::log10(at_cutoff);

		std::cout << "Low-pass at PSG rate: 100 Hz=" << at_dc
				<< ", 3390 Hz=" << at_cutoff << " (" << cutoff_db << " dB)\n";
		bool ok = true;
		ok &= check(at_dc > 0.99, "low-pass should pass 100 Hz unattenuated");
		ok &= check(cutoff_db < -2.5 && cutoff_db > -3.5,
				"low-pass should be about -3 dB at its cutoff");
		return ok;
	}

	// Peak-to-peak span of one second of PSG output at the given note period.
	int32_t psg_span(SoundDevice &device, uint16_t period)
	{
		device.write(0, 0, 0x80 | (period & 0x0f));
		device.write(0, 0, (period >> 4) & 0x3f);
		device.write(0, 0, 0x90); // channel 0 at full volume

		std::vector<WAVE_32BS> out(kSampleRate);
		int32_t low = INT32_MAX;
		int32_t high = INT32_MIN;
		for (uint32_t i = 0; i < kSampleRate; ++i)
		{
			out[i].L = 0;
			out[i].R = 0;
			device.get_sample(&out[i], 1);
			if (i < kSampleRate / 2)
				continue; // let the filter settle
			low = std::min(low, out[i].L);
			high = std::max(high, out[i].L);
		}
		return high - low;
	}

	bool test_device_lowpass()
	{
		// Tone period 0x008 is about 14 kHz, period 0x080 about 873 Hz.
		SoundDevice filtered;
		filtered.set_default_volume(SoundDevice::kPsgOutputVolume);
		filtered.init_sn76489(kSn76496Clock);
		filtered.set_rate(kSampleRate);
		const int32_t filtered_high = psg_span(filtered, 0x008);

		SoundDevice unfiltered;
		unfiltered.set_default_volume(SoundDevice::kPsgOutputVolume);
		unfiltered.set_lowpass(false, 0.0);
		unfiltered.init_sn76489(kSn76496Clock);
		unfiltered.set_rate(kSampleRate);
		const int32_t unfiltered_high = psg_span(unfiltered, 0x008);

		SoundDevice low_note;
		low_note.set_default_volume(SoundDevice::kPsgOutputVolume);
		low_note.init_sn76489(kSn76496Clock);
		low_note.set_rate(kSampleRate);
		const int32_t filtered_low = psg_span(low_note, 0x080);

		std::cout << "PSG spans: high filtered=" << filtered_high
				<< ", high unfiltered=" << unfiltered_high
				<< ", low filtered=" << filtered_low << '\n';
		bool ok = true;
		ok &= check(filtered_high * 3 < unfiltered_high,
				"the device filter should attenuate a high PSG tone");
		ok &= check(filtered_high * 3 < filtered_low,
				"a high PSG tone should end up quieter than a low one");
		ok &= check(filtered_low * 10 > unfiltered_high * 9,
				"a low PSG tone should pass the filter nearly untouched");
		return ok;
	}

	// Peak-to-peak span of the preview's left output over `frames`.
	int32_t preview_span(PreviewSynth &preview, uint32_t frames)
	{
		std::vector<WAVE_32BS> out(frames, WAVE_32BS{0, 0});
		preview.render(out.data(), static_cast<int>(frames));
		int32_t low = INT32_MAX;
		int32_t high = INT32_MIN;
		for (const auto &sample : out)
		{
			low = std::min(low, sample.L);
			high = std::max(high, sample.L);
		}
		return high - low;
	}

	bool test_preview_psg_envelope_end()
	{
		// A loop target past the data, and a sustain marker as the last byte.
		const std::vector<uint8_t> loop_out = {0x20, 0x02, 0x40};
		const std::vector<uint8_t> sustain_last = {0x20, 0x01};
		bool ok = true;
		for (const auto *env : {&loop_out, &sustain_last})
		{
			PreviewSynth preview;
			preview.init(kSampleRate);
			preview.load_psg(env->data(), static_cast<int>(env->size()));
			preview.set_mode(1);
			preview.note_on(60, 100);
			ok &= check(preview_span(preview, kSampleRate / 10) > 0, "the PSG preview note should sound");
			preview.note_off(60);
			preview_span(preview, kSampleRate / 5);
			ok &= check(preview_span(preview, kSampleRate / 5) == 0,
					"a PSG preview note should end where its envelope data ends");
		}
		return ok;
	}

	bool test_preview_psg_volume()
	{
		PreviewSynth preview;
		preview.init(kSampleRate);
		return check(preview.get_psg_output_volume() == SoundDevice::kPsgOutputVolume,
				"preview PSG volume should match playback's PSG resampler volume") &&
				check(preview.get_fm_output_volume() == SoundDevice::kDefaultOutputVolume,
						"preview FM volume should match playback's 0x100 resampler volume");
	}
}

int main()
{
	bool ok = true;
	ok &= test_gain_staging();
	ok &= test_chip_types();
	ok &= test_dc_blocker();
	ok &= test_lowpass_response();
	ok &= test_device_lowpass();
	ok &= test_preview_psg_volume();
	ok &= test_preview_psg_envelope_end();
	return ok ? 0 : 1;
}
