#include "vgm_audio_renderer.h"
#include "input.h"
#include "platform/md.h"
#include "ymfm_ym2612_device.h"
#include "mame_sn76496_device.h"

#include <stdexcept>
#include <algorithm>

namespace
{
	// VGM chip IDs used as device map keys
	constexpr int DEVID_SN76496 = 0x00;
	constexpr int DEVID_YM2612 = 0x02;
}

SoundDevice::SoundDevice()
		: resmpl{}, dev_init(false), resmpl_init(false), chip_type(CHIP_NONE),
		  sample_rate(0), volume(kDefaultOutputVolume), ym2612_chip_type(Ym2612ChipType::Ym2612)
{
}

SoundDevice::~SoundDevice()
{
	reset_device();
}

void SoundDevice::reset_device()
{
	if (resmpl_init)
	{
		Resmpl_Deinit(&resmpl);
		resmpl_init = false;
	}

	ymfm_ym2612.reset();
	mame_sn76496.reset();
	resmpl = {};
	dev_init = false;
	chip_type = CHIP_NONE;
}

void SoundDevice::set_default_volume(uint16_t vol)
{
	volume = vol;
}

uint16_t SoundDevice::get_default_volume() const
{
	return volume;
}

void SoundDevice::set_rate(uint32_t rate)
{
	sample_rate = rate;

	if (resmpl_init)
	{
		Resmpl_Deinit(&resmpl);
		resmpl_init = false;
	}

	if (dev_init)
	{
		if (chip_type == CHIP_YM2612)
		{
			resmpl.smpRateSrc = ymfm_ym2612->sample_rate();
			resmpl.StreamUpdate = &YmfmYm2612Device::stream_update;
			resmpl.su_DataPtr = ymfm_ym2612.get();
		}
		else if (chip_type == CHIP_SN76496)
		{
			resmpl.smpRateSrc = mame_sn76496->sample_rate();
			resmpl.StreamUpdate = &MameSn76496Device::stream_update;
			resmpl.su_DataPtr = mame_sn76496.get();
		}
		Resmpl_SetVals(&resmpl, 0xff, volume, sample_rate);
		Resmpl_Init(&resmpl);
		resmpl_init = true;
	}
}

void SoundDevice::init_sn76489(uint32_t freq, uint8_t lfsr_w, uint16_t lfsr_t)
{
	reset_device();

	mame_sn76496 = std::make_unique<MameSn76496Device>(freq, lfsr_w, lfsr_t);
	dev_init = true;
	chip_type = CHIP_SN76496;
}

void SoundDevice::init_ym2612(uint32_t freq)
{
	reset_device();

	ymfm_ym2612 = std::make_unique<YmfmYm2612Device>(freq, ym2612_chip_type);
	dev_init = true;
	chip_type = CHIP_YM2612;
}

void SoundDevice::write(uint16_t addr, uint16_t data)
{
	if (chip_type == CHIP_SN76496)
		mame_sn76496->write(static_cast<uint8_t>(data));
}

void SoundDevice::write(uint8_t port, uint16_t addr, uint16_t data)
{
	if (chip_type == CHIP_YM2612)
	{
		ymfm_ym2612->write(static_cast<uint8_t>(port << 1), static_cast<uint8_t>(addr));
		ymfm_ym2612->write(static_cast<uint8_t>((port << 1) + 1), static_cast<uint8_t>(data));
	}
	else if (chip_type == CHIP_SN76496)
	{
		mame_sn76496->write(static_cast<uint8_t>(data));
	}
}

void SoundDevice::get_sample(WAVE_32BS *output, int count)
{
	if (resmpl_init)
		Resmpl_Execute(&resmpl, count, output);
}

void SoundDevice::set_mute_mask(uint32_t mask)
{
	if (chip_type == CHIP_YM2612 && ymfm_ym2612)
		ymfm_ym2612->set_mute_mask(mask);
	else if (chip_type == CHIP_SN76496 && mame_sn76496)
		mame_sn76496->set_mute_mask(mask);
}

void SoundDevice::set_ym2612_chip_type(Ym2612ChipType chip_type_in)
{
	ym2612_chip_type = chip_type_in;
	if (ymfm_ym2612)
		ymfm_ym2612->set_chip_type(chip_type_in);
}

VgmAudioRenderer::VgmAudioRenderer(std::shared_ptr<Song> song, uint32_t start_position, bool log_messages)
		: sample_rate(1), delta_time(0), sample_delta(1), finished(false), log_messages(log_messages),
		  ym2612_chip_type(Ym2612ChipType::Ym2612), last_error_message(""), song(std::move(song))
{
	driver = this->song->get_platform()->get_driver(1, (VGM_Interface *)this);
	driver.get()->play_song(*this->song.get());
	if (start_position)
		driver.get()->skip_ticks(start_position);
}

VgmAudioRenderer::~VgmAudioRenderer()
{
}

std::shared_ptr<Driver> &VgmAudioRenderer::get_driver()
{
	return driver;
}

void VgmAudioRenderer::set_mute_mask(int chip_id, uint32_t mask)
{
	auto it = devices.find(chip_id);
	if (it != devices.end())
		it->second.set_mute_mask(mask);
}

void VgmAudioRenderer::set_ym2612_chip_type(Ym2612ChipType chip_type)
{
	ym2612_chip_type = chip_type;
	auto it = devices.find(DEVID_YM2612);
	if (it != devices.end())
		it->second.set_ym2612_chip_type(chip_type);
}

Ym2612ChipType VgmAudioRenderer::get_ym2612_chip_type() const
{
	return ym2612_chip_type;
}

void VgmAudioRenderer::setup_stream(uint32_t sample_rate)
{
	if (sample_rate == 0)
		sample_rate = 1;
	this->sample_rate = sample_rate;
	sample_delta = 1.0f / static_cast<float>(sample_rate);
	delta_time = 0;

	for (auto &device_pair : devices)
		device_pair.second.set_rate(sample_rate);
}

int VgmAudioRenderer::get_sample(WAVE_32BS *output, int count)
{
	try
	{
		auto *driver_ptr = driver.get();
		for (int i = 0; i < count; i++)
		{
			int max_steps = 100;
			delta_time += sample_delta;

			while (delta_time > 0)
			{
				double step = driver_ptr->play_step();
				delta_time -= step;
				if (!--max_steps)
					break;
			}

			// Update any dac streams
			for (auto &stream_pair : streams)
			{
				auto &stream = stream_pair.second;
				if (!stream.active)
					continue;
				stream.counter += stream.freq;
				while (stream.counter >= sample_rate)
				{
					devices[stream.chip_id].write(
							stream.port,
							stream.reg,
							datablocks[stream.db_id][stream.position]);
					stream.position++;
					stream.counter -= sample_rate;
					if (!--stream.length)
						stream.active = false;
				}
			}

			// Get sample from sound chips
			for (auto &device_pair : devices)
				device_pair.second.get_sample(&output[i], 1);

			if (!driver_ptr->is_playing())
				finished = true;
		}
	}
	catch (InputError &e)
	{
		handle_error(e.what());
	}
	catch (std::exception &e)
	{
		handle_error(e.what());
	}
	return count;
}

void VgmAudioRenderer::stop_playback()
{
	finished = true;
}

bool VgmAudioRenderer::is_finished() const
{
	return finished;
}

int VgmAudioRenderer::get_loop_count() const
{
	return driver ? driver->get_loop_count() : 0;
}

void VgmAudioRenderer::relink_song(std::shared_ptr<Song> new_song, uint32_t current_tick)
{
	if(!driver || !new_song)
		return;
	driver->relink_song(*new_song, current_tick);
	song = std::move(new_song);
	finished = false;
}

const std::string &VgmAudioRenderer::last_error() const
{
	return last_error_message;
}

void VgmAudioRenderer::handle_error(const char *str)
{
	last_error_message = str ? str : "";
	if (log_messages)
		printf("Playback error: %s\n", str);
	delta_time = -1000; // Prevent error from recurring
	finished = true;
}

void VgmAudioRenderer::write(uint8_t command, uint16_t port, uint16_t reg, uint16_t data)
{
	switch (command)
	{
	case 0x50:
		devices[DEVID_SN76496].write(reg, data);
		break;
	case 0x52:
		devices[DEVID_YM2612].write(port, reg, data);
		break;
	default:
		break;
	}
}

void VgmAudioRenderer::dac_setup(uint8_t sid, uint8_t chip_id, uint32_t port, uint32_t reg, uint8_t db_id)
{
	streams[sid].chip_id = chip_id;
	streams[sid].port = port;
	streams[sid].reg = reg;
	streams[sid].db_id = db_id;
	streams[sid].active = false;
}

void VgmAudioRenderer::dac_start(uint8_t sid, uint32_t start, uint32_t length, uint32_t freq)
{
	streams[sid].position = start;
	streams[sid].length = length;
	streams[sid].freq = freq;
	streams[sid].counter = sample_rate;
	streams[sid].active = true;
}

void VgmAudioRenderer::dac_stop(uint8_t sid)
{
	streams[sid].active = false;
}

void VgmAudioRenderer::poke32(uint32_t offset, uint32_t data)
{
	uint32_t clock = data & 0x3fffffff;
	switch (offset)
	{
	case 0x0c:
		devices[DEVID_SN76496].set_default_volume(SoundDevice::kPsgOutputVolume);
		devices[DEVID_SN76496].init_sn76489(clock);
		devices[DEVID_SN76496].set_rate(sample_rate);
		break;
	case 0x2c:
		devices[DEVID_YM2612].set_ym2612_chip_type(ym2612_chip_type);
		devices[DEVID_YM2612].init_ym2612(clock);
		devices[DEVID_YM2612].set_rate(sample_rate);
		break;
	default:
		if (log_messages)
			printf("EmuPlayerWeb poke %02x = %08x\n", offset, data);
		break;
	}
}

void VgmAudioRenderer::poke16(uint32_t offset, uint16_t data)
{
	if (log_messages)
		printf("EmuPlayerWeb poke %02x = %04x\n", offset, data);
}

void VgmAudioRenderer::poke8(uint32_t offset, uint8_t data)
{
	if (log_messages)
		printf("EmuPlayerWeb poke %02x = %02x\n", offset, data);
}

void VgmAudioRenderer::set_loop()
{
	if (log_messages)
		printf("EmuPlayerWeb set loop\n");
}

void VgmAudioRenderer::stop()
{
	if (log_messages)
		printf("EmuPlayerWeb stop\n");
	finished = true;
}

void VgmAudioRenderer::datablock(uint8_t dbtype, uint32_t dbsize, const uint8_t *db, uint32_t maxsize, uint32_t mask,
																 uint32_t flags, uint32_t offset)
{
	auto &block = datablocks[dbtype];
	block.resize(maxsize);
	std::copy_n(db, dbsize, block.begin() + offset);
}
