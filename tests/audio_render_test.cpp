#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "dc_blocker.h"
#include "preview_synth.h"
#include "vgm_audio_renderer.h"
#include "ymfm_ym2612_device.h"

namespace
{
	constexpr uint32_t kYm2612Clock = 7670454;
	constexpr uint32_t kSampleRate = 44100;

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
	ok &= test_preview_psg_volume();
	return ok ? 0 : 1;
}
