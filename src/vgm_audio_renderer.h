#pragma once

#include <memory>
#include <map>
#include <vector>
#include <string>

#include <resampler/EmuStructs.h>
#include <resampler/Resampler.h>

#include "vgm.h"
#include "song.h"
#include "driver.h"
#include "ymfm_ym2612_device.h"

class MameSn76496Device;

class SoundDevice
{
public:
	// Half of what the bipolar MAME core's level implies: real SN76489 output is
	// unipolar (0..+vol) behind an AC-coupling cap, so hardware peak-to-peak is
	// vol, not 2*vol. 0x40 matches blastem's PSG:FM balance within 0.6 dB.
	static constexpr uint16_t kPsgOutputVolume = 0x40;
	static constexpr uint16_t kDefaultOutputVolume = 0x100;

	SoundDevice();
	~SoundDevice();

	void set_default_volume(uint16_t vol);
	void set_rate(uint32_t rate);
	void init_sn76489(uint32_t freq,
										uint8_t lfsr_w = 0x10,
										uint16_t lfsr_t = 0x09);
	void init_ym2612(uint32_t freq);
	void write(uint16_t addr, uint16_t data);
	void write(uint8_t port, uint16_t addr, uint16_t data);
	void get_sample(WAVE_32BS *output, int count);
	void set_mute_mask(uint32_t mask);
	void set_ym2612_chip_type(Ym2612ChipType chip_type);
	uint16_t get_default_volume() const;

private:
	void reset_device();

	RESMPL_STATE resmpl;
	bool dev_init;
	bool resmpl_init;
	enum
	{
		CHIP_NONE = 0,
		CHIP_SN76496,
		CHIP_YM2612,
	} chip_type;
	uint32_t sample_rate;
	uint16_t volume;
	Ym2612ChipType ym2612_chip_type;
	std::unique_ptr<YmfmYm2612Device> ymfm_ym2612;
	std::unique_ptr<MameSn76496Device> mame_sn76496;
};

class VgmAudioRenderer : private VGM_Interface
{
public:
	VgmAudioRenderer(std::shared_ptr<Song> song, uint32_t start_position = 0, bool log_messages = true);
	~VgmAudioRenderer();

	std::shared_ptr<Driver> &get_driver();
	void setup_stream(uint32_t sample_rate);
	uint32_t get_sample_rate() const { return static_cast<uint32_t>(sample_rate); }
	int get_sample(WAVE_32BS *output, int count);
	void stop_playback();
	bool is_finished() const;
	const std::string &last_error() const;
	void set_mute_mask(int chip_id, uint32_t mask);
	void set_ym2612_chip_type(Ym2612ChipType chip_type);
	Ym2612ChipType get_ym2612_chip_type() const;
	int get_loop_count() const;
	//! Hot-swap to a freshly compiled song without resetting the chip
	//! emulators or the driver's channel objects. Forwards to
	//! Driver::relink_song(); preserves currently-sounding notes.
	void relink_song(std::shared_ptr<Song> new_song, uint32_t current_tick);

private:
	void handle_error(const char *str);
	void write(uint8_t command, uint16_t port, uint16_t reg, uint16_t data) override;
	void dac_setup(uint8_t sid, uint8_t chip_id, uint32_t port, uint32_t reg, uint8_t db_id) override;
	void dac_start(uint8_t sid, uint32_t start, uint32_t length, uint32_t freq) override;
	void dac_stop(uint8_t sid) override;
	void poke32(uint32_t offset, uint32_t data) override;
	void poke16(uint32_t offset, uint16_t data) override;
	void poke8(uint32_t offset, uint8_t data) override;
	void set_loop() override;
	void stop() override;
	void datablock(
			uint8_t dbtype,
			uint32_t dbsize,
			const uint8_t *db,
			uint32_t maxsize,
			uint32_t mask = 0xffffffff,
			uint32_t flags = 0,
			uint32_t offset = 0) override;

	struct DacStream
	{
		uint8_t chip_id;
		uint8_t port;
		uint8_t reg;
		uint8_t db_id;
		bool active;
		uint32_t position;
		uint32_t length;
		uint32_t freq;
		int32_t counter;
	};

	int sample_rate;
	float delta_time;
	float sample_delta;
	bool finished;
	bool log_messages;
	Ym2612ChipType ym2612_chip_type;
	std::string last_error_message;

	std::map<int, SoundDevice> devices;
	std::map<int, std::vector<uint8_t>> datablocks;
	std::map<int, DacStream> streams;

	std::shared_ptr<Driver> driver;
	std::shared_ptr<Song> song;
};
