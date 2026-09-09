#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <resampler/EmuStructs.h>
#include <resampler/Resampler.h>

#include "ymfm_ym2612_device.h"

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

	// Set mode: 0=FM, 1=PSG tone, 2=PSG noise (mode 0),
	//           3=PSG noise (mode 1/white), 4=PSG noise (mode 2/periodic)
	void set_mode(int mode);

	// Monophonic preview: a new note takes over the same voice and cuts the
	// previous note instead of allocating another one.
	void set_mono(bool m);

	// Set operator mask for FM key-on (4 bits: bit0=OP1..bit3=OP4, default 0x0f=all)
	void set_fm_op_mask(uint8_t mask);
	void set_ym2612_chip_type(Ym2612ChipType chip_type);
	//! Low-pass both chips at their native rate, before resampling.
	void set_lowpass(bool enabled, double cutoff_hz);
	uint16_t get_fm_output_volume() const;
	uint16_t get_psg_output_volume() const;

	// Note on/off (MIDI note number, 0-127)
	void note_on(uint8_t midi_note, uint8_t velocity);
	void note_on_attenuation(uint8_t midi_note, uint8_t tl_attenuation);
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
	void fm_set_volume_attenuation(int ch, uint8_t tl_attenuation);
	void fm_key_on(int ch);
	void fm_key_off(int ch);
	int fm_allocate_voice();
	int fm_mono_voice(uint8_t midi_note);
	void note_on_impl(uint8_t midi_note, uint8_t volume, bool direct_attenuation);

	// PSG helpers
	void psg_write(uint8_t data);
	void psg_set_pitch(int ch, uint8_t midi_note);
	void psg_set_volume(int ch, uint8_t vol);
	void psg_update_envelopes();
	int psg_allocate_voice();
	int psg_mono_voice(uint8_t midi_note);

	static uint16_t calc_fm_pitch(uint8_t midi_note, int transpose);
	static uint16_t calc_psg_pitch(uint8_t midi_note);

	std::unique_ptr<SoundDevice> ym2612;
	std::unique_ptr<SoundDevice> sn76496;
	bool initialized;

	// Current mode: 0=FM, 1=PSG tone, 2=PSG noise(mode0),
	//               3=PSG noise(mode1/white), 4=PSG noise(mode2/periodic)
	int mode;

	// Monophonic preview flag
	bool mono;
	// In mono mode the key-on for voice 0 is written by render() after one
	// frame has been clocked with the key off: the chip samples the key state
	// once per frame, so an off/on pair written back to back would not
	// restart the envelope.
	bool fm_key_on_pending;

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
	uint8_t fm_op_mask; // 4-bit operator mask for key-on (bit0=OP1..bit3=OP4)
	Ym2612ChipType ym2612_chip_type;
	bool lowpass_enabled;
	double lowpass_cutoff;

	// PSG state
	struct PsgVoice
	{
		bool active;
		uint8_t midi_note;
		int env_pos;
		int env_delay;
		bool env_keyoff;
		// Set from note off until the voice ends, where env_keyoff is cleared by
		// the sustain marker: a loop reached in this state has no later key off
		// to end it.
		bool env_released;
		// Loops taken since the release began.
		uint8_t env_loops;
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
	uint32_t silent_counter;  // consecutive silent samples rendered, saturated at sample_rate / 2
	bool held_notes[128];     // bitmap of currently-held MIDI notes
};
