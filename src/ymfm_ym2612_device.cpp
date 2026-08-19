#include "ymfm_ym2612_device.h"

#include <algorithm>
#include <ymfm_opn.h>

class YmfmYm2612Interface : public ymfm::ymfm_interface
{
};

class YmfmYm2612Chip : public ymfm::ym2612
{
public:
	using ymfm::ym2612::ym2612;

	void generate_masked(output_data *output, uint32_t numsamples, uint32_t chanmask,
			Ym2612ChipType chip_type)
	{
		chanmask &= fm_engine::ALL_CHANNELS;
		if (chip_type == Ym2612ChipType::Ym2612 && chanmask == fm_engine::ALL_CHANNELS)
		{
			generate(output, numsamples);
			return;
		}
		for (uint32_t samp = 0; samp < numsamples; samp++, output++)
		{
			m_fm.clock(fm_engine::ALL_CHANNELS);

			const int last_fm_channel = m_dac_enable ? 5 : 6;
			if (chip_type == Ym2612ChipType::Ym3438)
			{
				if (!m_dac_enable)
				{
					m_fm.output(output->clear(), 5, 256, chanmask);
				}
				else
				{
					const int32_t dacval = int16_t(m_dac_data << 7) >> 7;
					output->data[0] = (chanmask & (1u << 5)) != 0 &&
							m_fm.regs().ch_output_0(0x102) ? dacval : 0;
					output->data[1] = (chanmask & (1u << 5)) != 0 &&
							m_fm.regs().ch_output_1(0x102) ? dacval : 0;
					m_fm.output(*output, 5, 256, chanmask & ~(1u << 5));
				}

				output->data[0] = (output->data[0] * 128) / 6;
				output->data[1] = (output->data[1] * 128) / 6;
				continue;
			}

			output->clear();
			output_data temp;
			for (int chan = 0; chan < last_fm_channel; chan++)
			{
				if ((chanmask & (1u << chan)) == 0)
					continue;

				m_fm.output(temp.clear(), 5, 256, 1u << chan);
				output->data[0] += dac_discontinuity(temp.data[0]);
				output->data[1] += dac_discontinuity(temp.data[1]);
			}

			if (m_dac_enable && (chanmask & (1u << 5)) != 0)
			{
				const int32_t dacval = dac_discontinuity(int16_t(m_dac_data << 7) >> 7);
				output->data[0] += m_fm.regs().ch_output_0(0x102) ? dacval : dac_discontinuity(0);
				output->data[1] += m_fm.regs().ch_output_1(0x102) ? dacval : dac_discontinuity(0);
			}

			output->data[0] = (output->data[0] * 128) * 64 / (6 * 65);
			output->data[1] = (output->data[1] * 128) * 64 / (6 * 65);
		}
	}
};

YmfmYm2612Device::YmfmYm2612Device(uint32_t clock_in, Ym2612ChipType chip_type_in)
	: clock(clock_in), mute_mask(0), chip_type(chip_type_in), port0_address(0), key_state{},
	  interface(std::make_unique<YmfmYm2612Interface>()),
	  chip(std::make_unique<YmfmYm2612Chip>(*interface))
{
	chip->reset();
}

YmfmYm2612Device::~YmfmYm2612Device() = default;

uint32_t YmfmYm2612Device::sample_rate() const
{
	return chip->sample_rate(clock);
}

void YmfmYm2612Device::reset()
{
	chip->reset();
	std::fill(std::begin(key_state), std::end(key_state), 0);
}

void YmfmYm2612Device::write(uint8_t offset, uint8_t data)
{
	// Track port 0 address register to detect key-on register (0x28) writes
	if (offset == 0)
	{
		port0_address = data;
	}
	else if (offset == 1 && port0_address == 0x28)
	{
		// Register 0x28: key-on/off
		// Bits [2:0] = channel (0-2 for port 0, 4-6 for port 1)
		// Bits [7:4] = operator enable mask
		const uint8_t ch_raw = data & 0x07;
		const uint8_t new_ops = data & 0xF0;
		const uint8_t prev_ops = key_state[ch_raw & 0x07];

		// Find operators that are being newly keyed on (0→1 transition).
		// Operators already on must not be disturbed (FM3 SP mode).
		const uint8_t rising = new_ops & ~prev_ops;

		if (rising != 0)
		{
			// Force key-off only for the operators about to be keyed on,
			// while preserving any operators that are already sounding.
			// This lets ymfm see a 1→0→1 transition on just the new
			// operators so their envelopes restart cleanly.
			chip->write(0, 0x28);
			chip->write(1, (prev_ops & ~rising) | ch_raw);
			YmfmYm2612Chip::output_data dummy;
			chip->generate(&dummy, 1);
		}

		key_state[ch_raw & 0x07] = new_ops;
	}

	chip->write(offset, data);
}

void YmfmYm2612Device::render(DEV_SMPL **outputs, uint32_t samples)
{
	if (outputs == nullptr || outputs[0] == nullptr || outputs[1] == nullptr)
		return;

	const uint32_t active_mask = YmfmYm2612Chip::fm_engine::ALL_CHANNELS & ~mute_mask;
	YmfmYm2612Chip::output_data sample{};
	for (uint32_t index = 0; index < samples; index++)
	{
		chip->generate_masked(&sample, 1, active_mask, chip_type);
		outputs[0][index] = sample.data[0];
		outputs[1][index] = sample.data[1];
	}
}

void YmfmYm2612Device::set_mute_mask(uint32_t mask)
{
	mute_mask = mask & YmfmYm2612Chip::fm_engine::ALL_CHANNELS;
}

void YmfmYm2612Device::set_chip_type(Ym2612ChipType chip_type_in)
{
	chip_type = chip_type_in;
}

Ym2612ChipType YmfmYm2612Device::get_chip_type() const
{
	return chip_type;
}

void YmfmYm2612Device::stream_update(void *info, UINT32 samples, DEV_SMPL **outputs)
{
	if (info == nullptr)
		return;
	static_cast<YmfmYm2612Device *>(info)->render(outputs, samples);
}
