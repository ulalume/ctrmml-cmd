#pragma once

#include <memory>
#include <map>
#include <vector>
#include <string>

#include <resampler/EmuStructs.h>
#include <resampler/Resampler.h>

#include "lowpass_filter.h"
#include "vgm.h"
#include "song.h"
#include "driver.h"
#include "ymfm_ym2612_device.h"

class MameSn76496Device;

class SoundDevice
{
public:
	// PSG:FM ratio matched to blastem's mixer (unipolar 0..vol per PSG channel).
	static constexpr uint16_t kPsgOutputVolume = 0x49;
	static constexpr uint16_t kDefaultOutputVolume = 0x100;

	SoundDevice();
	~SoundDevice();

	void set_default_volume(uint16_t vol);
	void set_rate(uint32_t rate);
	void init_sn76489(uint32_t freq,
										uint8_t lfsr_w = 0x10,
										uint16_t lfsr_t = 0x09);
	void init_ym2612(uint32_t freq);
	void write(uint8_t port, uint16_t addr, uint16_t data);
	void get_sample(WAVE_32BS *output, int count);
	void set_mute_mask(uint32_t mask);
	void set_ym2612_chip_type(Ym2612ChipType chip_type);
	//! Low-pass the chip output at its native rate, before resampling.
	void set_lowpass(bool enabled, double cutoff_hz);
	uint16_t get_default_volume() const;

private:
	void reset_device();
	//! Resampler callback: renders the chip, then low-passes the chunk.
	static void stream_update(void *info, UINT32 samples, DEV_SMPL **outputs);

	RESMPL_STATE resmpl;
	bool resmpl_init;
	enum
	{
		CHIP_NONE = 0,
		CHIP_SN76496,
		CHIP_YM2612,
	} chip_type;
	uint16_t volume;
	Ym2612ChipType ym2612_chip_type;
	std::unique_ptr<YmfmYm2612Device> ymfm_ym2612;
	std::unique_ptr<MameSn76496Device> mame_sn76496;
	LowPassFilter lowpass;
	bool lowpass_enabled;
	double lowpass_cutoff;
};

class VgmAudioRenderer : private VGM_Interface
{
public:
	VgmAudioRenderer(std::shared_ptr<Song> song, uint32_t start_position = 0, bool log_messages = true);
	~VgmAudioRenderer();

	std::shared_ptr<Driver> &get_driver();
	void setup_stream(uint32_t sample_rate);
	//! Adds `count` rendered samples to `output`.
	void get_sample(WAVE_32BS *output, int count);
	void stop_playback();
	bool is_finished() const;
	const std::string &last_error() const;
	void set_mute_mask(int chip_id, uint32_t mask);
	void set_ym2612_chip_type(Ym2612ChipType chip_type);
	//! Low-pass every chip at its own native rate, before resampling.
	void set_lowpass(bool enabled, double cutoff_hz);
	//! Finish playback once PCM output ends: a DAC stream reaches the end of
	//! its sample, or the DAC is switched off after being enabled.
	void set_stop_at_pcm_end(bool enabled);
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

	SoundDevice *device(int chip_id);

	struct DacStream
	{
		SoundDevice *device; // null for a chip this renderer does not emulate
		const std::vector<uint8_t> *block;
		uint8_t port;
		uint8_t reg;
		bool active;
		uint32_t position;
		uint32_t length;
		uint32_t freq;
		int32_t counter;
	};

	//! Stops a stream whose length or data block ran out.
	void end_dac_stream(DacStream &stream);

	int sample_rate;
	float delta_time;
	float sample_delta;
	bool finished;
	bool log_messages;
	bool lowpass_enabled;
	double lowpass_cutoff;
	bool stop_at_pcm_end;
	bool dac_enabled;
	Ym2612ChipType ym2612_chip_type;
	std::string last_error_message;

	SoundDevice sn76496; // VGM chip id 0x00
	SoundDevice ym2612;  // VGM chip id 0x02
	std::map<int, std::vector<uint8_t>> datablocks;
	std::map<int, DacStream> streams;

	std::shared_ptr<Driver> driver;
	std::shared_ptr<Song> song;
};
