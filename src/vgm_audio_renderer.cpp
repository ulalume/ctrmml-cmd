#include "vgm_audio_renderer.h"
#include "input.h"
#include "platform/md.h"

#include <emu/EmuCores.h>
#include <emu/cores/sn764intf.h>

#include <stdexcept>
#include <algorithm>

SoundDevice::SoundDevice()
		: dev_init(false), resmpl_init(false), write_type(SoundDevice::NONE), volume(0x100), write_a8d8(nullptr)
{
}

SoundDevice::~SoundDevice()
{
	if (resmpl_init)
	{
		Resmpl_Deinit(&resmpl);
	}
	if (dev_init)
	{
		SndEmu_Stop(&dev);
	}
}

void SoundDevice::set_default_volume(uint16_t vol)
{
	volume = vol;
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
		Resmpl_SetVals(&resmpl, 0xff, volume, sample_rate);
		Resmpl_DevConnect(&resmpl, &dev);
		Resmpl_Init(&resmpl);
		resmpl_init = true;
	}
}

void SoundDevice::init_sn76489(uint32_t freq, uint8_t lfsr_w, uint16_t lfsr_t)
{
	DEV_GEN_CFG dev_cfg{};
	SN76496_CFG sn_cfg{};

	dev_cfg.emuCore = 0;
	dev_cfg.srMode = DEVRI_SRMODE_NATIVE;
	dev_cfg.flags = 0x00;
	dev_cfg.clock = freq;
	dev_cfg.smplRate = 44100;
	sn_cfg._genCfg = dev_cfg;
	sn_cfg.shiftRegWidth = lfsr_w;
	sn_cfg.noiseTaps = lfsr_t;
	sn_cfg.negate = 0;
	sn_cfg.stereo = 0;
	sn_cfg.clkDiv = 8;
	sn_cfg.segaPSG = 1;
	sn_cfg.t6w28_tone = NULL;

	uint8_t status = SndEmu_Start(DEVID_SN76496, (DEV_GEN_CFG *)&sn_cfg, &dev);
	if (status)
		throw std::runtime_error("SoundDevice::init_sn76489");

	SndEmu_GetDeviceFunc(dev.devDef, RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, (void **)&write_a8d8);
	write_type = SoundDevice::A8D8;

	dev.devDef->Reset(dev.dataPtr);

	dev_init = true;
}

void SoundDevice::init_ym2612(uint32_t freq)
{
	DEV_GEN_CFG dev_cfg;

	dev_cfg.emuCore = FCC_NUKE;
	dev_cfg.srMode = DEVRI_SRMODE_NATIVE;
	dev_cfg.flags = 0x00;
	dev_cfg.clock = freq;
	dev_cfg.smplRate = 44100;

	uint8_t status = SndEmu_Start(DEVID_YM2612, (DEV_GEN_CFG *)&dev_cfg, &dev);
	if (status)
		throw std::runtime_error("SoundDevice::init_ym2612");

	SndEmu_GetDeviceFunc(dev.devDef, RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, (void **)&write_a8d8);
	write_type = SoundDevice::P1A8D8;

	dev.devDef->Reset(dev.dataPtr);

	dev_init = true;
}

void SoundDevice::write(uint16_t addr, uint16_t data)
{
	switch (write_type)
	{
	default:
		break;
	case SoundDevice::A8D8:
		write_a8d8(dev.dataPtr, addr, data);
		break;
	}
}

void SoundDevice::write(uint8_t port, uint16_t addr, uint16_t data)
{
	switch (write_type)
	{
	default:
		write(addr, data);
		break;
	case SoundDevice::P1A8D8:
		write_a8d8(dev.dataPtr, (port << 1), addr);
		write_a8d8(dev.dataPtr, (port << 1) + 1, data);
		break;
	}
}

void SoundDevice::get_sample(WAVE_32BS *output, int count)
{
	if (resmpl_init)
		Resmpl_Execute(&resmpl, count, output);
}

void SoundDevice::set_mute_mask(uint32_t mask)
{
	if (dev_init)
		dev.devDef->SetMuteMask(dev.dataPtr, mask);
}

VgmAudioRenderer::VgmAudioRenderer(std::shared_ptr<Song> song, uint32_t start_position, bool log_messages)
		: sample_rate(1), delta_time(0), sample_delta(1), finished(false), log_messages(log_messages), last_error_message(""), song(std::move(song))
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
		devices[DEVID_SN76496].set_default_volume(0x80);
		devices[DEVID_SN76496].init_sn76489(clock);
		devices[DEVID_SN76496].set_rate(sample_rate);
		break;
	case 0x2c:
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
