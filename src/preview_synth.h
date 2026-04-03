#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <emu/EmuStructs.h>
#include <emu/Resampler.h>

class SoundDevice;

class PreviewSynth
{
public:
	PreviewSynth();
	~PreviewSynth();

	void init(uint32_t sample_rate);
	void deinit();

	// Load FM instrument from 30-byte MDSDRV data_bank binary
	void load_fm(const uint8_t *data, int len);
	// Load PSG envelope from compiled MDSDRV binary
	void load_psg(const uint8_t *data, int len);

	// Set mode: 0=FM, 1=PSG tone, 2=PSG noise (mode 0), 3=PSG noise (mode 1/white)
	void set_mode(int mode);

	// Note on/off (MIDI note number, 0-127)
	void note_on(uint8_t midi_note, uint8_t velocity);
	void note_off(uint8_t midi_note);
	void all_notes_off();

	// Render audio (additive: adds to existing output)
	void render(WAVE_32BS *output, int frames);
	bool is_active() const;

private:
	// FM helpers
	void fm_write(uint8_t port, uint8_t reg, uint8_t data);
	void fm_load_instrument(int ch);
	void fm_set_pitch(int ch, uint8_t midi_note);
	void fm_set_volume(int ch, uint8_t velocity);
	void fm_key_on(int ch);
	void fm_key_off(int ch);
	int fm_allocate_voice();

	// PSG helpers
	void psg_write(uint8_t data);
	void psg_set_pitch(int ch, uint8_t midi_note);
	void psg_set_volume(int ch, uint8_t vol);
	void psg_update_envelopes();
	int psg_allocate_voice();

	static uint16_t calc_fm_pitch(uint8_t midi_note, int transpose);
	static uint16_t calc_psg_pitch(uint8_t midi_note);

	std::unique_ptr<SoundDevice> ym2612;
	std::unique_ptr<SoundDevice> sn76496;
	bool initialized;

	// Current mode: 0=FM, 1=PSG tone, 2=PSG noise(mode0), 3=PSG noise(mode1/white)
	int mode;

	// FM state
	struct FmVoice
	{
		bool active;
		uint8_t midi_note;
		uint32_t age;
	};
	FmVoice fm_voices[6];
	uint8_t fm_instrument[30];
	bool fm_instrument_loaded;
	int fm_transpose;

	// PSG state
	struct PsgVoice
	{
		bool active;
		uint8_t midi_note;
		int env_pos;
		int env_delay;
		bool env_keyoff;
		uint8_t env_vol;
	};
	PsgVoice psg_voices[3];
	PsgVoice psg_noise_voice; // ch3 (noise channel)
	std::vector<uint8_t> psg_envelope;
	bool psg_envelope_loaded;

	uint32_t sample_rate;
	uint32_t psg_tick_counter;
	uint32_t psg_tick_period; // samples per envelope tick
	uint32_t age_counter;
};
