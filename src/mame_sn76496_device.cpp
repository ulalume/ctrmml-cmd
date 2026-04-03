#include "mame_sn76496_device.h"

#include <cstring>
#include <stdexcept>

#include <resampler/EmuStructs.h>
#include "sn76496/sn764intf.h"
#include "sn76496/sn76496.h"

MameSn76496Device::MameSn76496Device(uint32_t clock, uint8_t lfsr_w, uint16_t lfsr_t)
	: rate(0), chip_ptr(nullptr)
{
	SN76496_CFG cfg;
	std::memset(&cfg, 0, sizeof(cfg));
	cfg._genCfg.emuCore = 0;
	cfg._genCfg.srMode = DEVRI_SRMODE_NATIVE;
	cfg._genCfg.flags = 0x00;
	cfg._genCfg.clock = clock;
	cfg._genCfg.smplRate = 44100;
	cfg.shiftRegWidth = lfsr_w;
	cfg.noiseTaps = lfsr_t;
	cfg.negate = 1;
	cfg.stereo = 0;
	cfg.clkDiv = 8;
	cfg.segaPSG = 1;
	cfg.t6w28_tone = nullptr;

	DEV_INFO dev_info;
	std::memset(&dev_info, 0, sizeof(dev_info));

	// devDef_SN76496_MAME.Start is the first function pointer in the DEV_DEF
	UINT8 status = devDef_SN76496_MAME.Start((DEV_GEN_CFG *)&cfg, &dev_info);
	if (status)
		throw std::runtime_error("MameSn76496Device: failed to start");

	chip_ptr = dev_info.dataPtr;
	rate = dev_info.sampleRate;

	devDef_SN76496_MAME.Reset(chip_ptr);
}

MameSn76496Device::~MameSn76496Device()
{
	if (chip_ptr)
		devDef_SN76496_MAME.Stop(chip_ptr);
}

uint32_t MameSn76496Device::sample_rate() const
{
	return rate;
}

void MameSn76496Device::reset()
{
	if (chip_ptr)
		devDef_SN76496_MAME.Reset(chip_ptr);
}

void MameSn76496Device::write(uint8_t data)
{
	if (!chip_ptr || !devDef_SN76496_MAME.rwFuncs)
		return;
	// rwFuncs[0] is the A8D8 write function (sn76496_w_mame)
	auto write_fn = (void (*)(void *, UINT8, UINT8))devDef_SN76496_MAME.rwFuncs[0].funcPtr;
	if (write_fn)
		write_fn(chip_ptr, SN76496_W_REG, data);
}

void MameSn76496Device::render(DEV_SMPL **outputs, uint32_t samples)
{
	if (!chip_ptr)
		return;
	devDef_SN76496_MAME.Update(chip_ptr, samples, outputs);
}

void MameSn76496Device::set_mute_mask(uint32_t mask)
{
	if (chip_ptr && devDef_SN76496_MAME.SetMuteMask)
		devDef_SN76496_MAME.SetMuteMask(chip_ptr, mask);
}

void MameSn76496Device::stream_update(void *info, UINT32 samples, DEV_SMPL **outputs)
{
	if (info == nullptr)
		return;
	static_cast<MameSn76496Device *>(info)->render(outputs, samples);
}
