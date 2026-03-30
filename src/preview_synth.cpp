#include "preview_synth.h"
#include "vgm_audio_renderer.h"

#include <algorithm>
#include <cstring>

// FM frequency table (same as md.cpp)
static const uint16_t fm_freqtab[13] = {
	644, 681, 722, 765, 810, 858, 910, 964, 1021, 1081, 1146, 1214, 1288};

// PSG frequency table (same as md.cpp)
static const uint16_t psg_freqtab[13] = {
	1710, 1614, 1524, 1438, 1357, 1281, 1209, 1141, 1077, 1017, 960, 906, 855};

// Algorithm → carrier operator mask (which ops are carriers)
// Bit order: OP4(3) OP3(2) OP2(1) OP1(0)
static const uint8_t alg_carrier_mask[8] = {
	0x08, // ALG 0: OP4 only
	0x08, // ALG 1: OP4 only
	0x08, // ALG 2: OP4 only
	0x08, // ALG 3: OP4 only
	0x0A, // ALG 4: OP2 + OP4
	0x0E, // ALG 5: OP2 + OP3 + OP4
	0x0E, // ALG 6: OP2 + OP3 + OP4
	0x0F, // ALG 7: all carriers
};

// Mega Drive clock frequencies
static const uint32_t YM2612_CLOCK = 7670454;
static const uint32_t SN76496_CLOCK = 3579545;

PreviewSynth::PreviewSynth()
	: initialized(false), mode(0),
	  fm_instrument_loaded(false), fm_transpose(0),
	  psg_envelope_loaded(false),
	  sample_rate(44100), psg_tick_counter(0), psg_tick_period(294),
	  age_counter(0)
{
	std::memset(fm_voices, 0, sizeof(fm_voices));
	std::memset(fm_instrument, 0, sizeof(fm_instrument));
	std::memset(psg_voices, 0, sizeof(psg_voices));
}

PreviewSynth::~PreviewSynth()
{
	deinit();
}

void PreviewSynth::init(uint32_t rate)
{
	deinit();

	sample_rate = rate;
	psg_tick_period = rate / 150; // MDSDRV default sequencer rate
	psg_tick_counter = 0;
	age_counter = 0;

	ym2612 = std::make_unique<SoundDevice>();
	sn76496 = std::make_unique<SoundDevice>();

	ym2612->init_ym2612(YM2612_CLOCK);
	ym2612->set_rate(rate);

	sn76496->init_sn76489(SN76496_CLOCK);
	sn76496->set_rate(rate);

	// Silence all PSG channels
	for (int ch = 0; ch < 4; ch++)
		psg_set_volume(ch, 15); // 15 = silent

	initialized = true;
}

void PreviewSynth::deinit()
{
	ym2612.reset();
	sn76496.reset();
	initialized = false;
	fm_instrument_loaded = false;
	psg_envelope_loaded = false;
	std::memset(fm_voices, 0, sizeof(fm_voices));
	std::memset(psg_voices, 0, sizeof(psg_voices));
}

// ---------------------------------------------------------------------------
// FM
// ---------------------------------------------------------------------------

void PreviewSynth::fm_write(uint8_t port, uint8_t reg, uint8_t data)
{
	if (!ym2612)
		return;
	ym2612->write(port, reg, data);
}

void PreviewSynth::load_fm(const uint8_t *data, int len)
{
	if (len < 30)
		return;
	std::memcpy(fm_instrument, data, 30);
	fm_transpose = (static_cast<int>(fm_instrument[29] >> 1)) - 24;
	fm_instrument_loaded = true;
}

void PreviewSynth::fm_load_instrument(int ch)
{
	uint8_t port = ch / 3;
	uint8_t id = ch % 3;

	// Silence all operators (TL = max)
	for (int op = 0; op < 4; op++)
		fm_write(port, 0x40 + op * 4 + id, 0x7f);

	// Key off
	fm_write(0, 0x28, id | (port << 2));

	// Load operator params
	for (int op = 0; op < 4; op++)
	{
		fm_write(port, 0x30 + op * 4 + id, fm_instrument[0 + op]);  // DT/MUL
		fm_write(port, 0x50 + op * 4 + id, fm_instrument[4 + op]);  // KS/AR
		fm_write(port, 0x60 + op * 4 + id, fm_instrument[8 + op]);  // AM/DR
		fm_write(port, 0x70 + op * 4 + id, fm_instrument[12 + op]); // SR
		fm_write(port, 0x80 + op * 4 + id, fm_instrument[16 + op]); // SL/RR
		fm_write(port, 0x90 + op * 4 + id, fm_instrument[20 + op]); // SSG-EG
	}

	// FB/ALG
	fm_write(port, 0xb0 + id, fm_instrument[28]);

	// Panning: both L+R enabled
	fm_write(port, 0xb4 + id, 0xc0);
}

uint16_t PreviewSynth::calc_fm_pitch(uint8_t midi_note, int transpose)
{
	int note = static_cast<int>(midi_note) - 12 + transpose; // MIDI 12 = C0 in ctrmml
	if (note < 0)
		note = 0;
	if (note > 95)
		note = 95;

	uint8_t octave = note / 12;
	uint8_t semitone = note % 12;
	uint16_t fnum = fm_freqtab[semitone];
	return fnum | ((octave & 7) << 11);
}

void PreviewSynth::fm_set_pitch(int ch, uint8_t midi_note)
{
	uint8_t port = ch / 3;
	uint8_t id = ch % 3;
	uint16_t pitch = calc_fm_pitch(midi_note, fm_transpose);

	// High byte first (block + fnum high bits)
	fm_write(port, 0xa4 + id, (pitch >> 8) & 0xff);
	fm_write(port, 0xa0 + id, pitch & 0xff);
}

void PreviewSynth::fm_set_volume(int ch, uint8_t velocity)
{
	uint8_t port = ch / 3;
	uint8_t id = ch % 3;
	uint8_t alg = fm_instrument[28] & 7;
	uint8_t carrier_mask = alg_carrier_mask[alg];

	// Convert velocity (0-127) to TL attenuation adjustment
	// velocity 127 = no attenuation, velocity 0 = max attenuation
	int vol_adj = (127 - velocity) >> 1; // 0..63

	for (int op = 0; op < 4; op++)
	{
		uint8_t tl = fm_instrument[24 + op];
		if (carrier_mask & (1 << op))
		{
			// Carrier: apply velocity
			int adj_tl = tl + vol_adj;
			if (adj_tl > 127)
				adj_tl = 127;
			tl = static_cast<uint8_t>(adj_tl);
		}
		fm_write(port, 0x40 + op * 4 + id, tl);
	}
}

void PreviewSynth::fm_key_on(int ch)
{
	uint8_t port = ch / 3;
	uint8_t id = ch % 3;
	// Key on all 4 operators
	fm_write(0, 0x28, 0xf0 | id | (port << 2));
}

void PreviewSynth::fm_key_off(int ch)
{
	uint8_t port = ch / 3;
	uint8_t id = ch % 3;
	fm_write(0, 0x28, id | (port << 2));
}

int PreviewSynth::fm_allocate_voice()
{
	// Find free voice
	int oldest_free = -1;
	uint32_t oldest_free_age = UINT32_MAX;
	int oldest_active = -1;
	uint32_t oldest_active_age = UINT32_MAX;

	for (int i = 0; i < 6; i++)
	{
		if (!fm_voices[i].active)
		{
			if (fm_voices[i].age < oldest_free_age)
			{
				oldest_free = i;
				oldest_free_age = fm_voices[i].age;
			}
		}
		else
		{
			if (fm_voices[i].age < oldest_active_age)
			{
				oldest_active = i;
				oldest_active_age = fm_voices[i].age;
			}
		}
	}

	if (oldest_free >= 0)
		return oldest_free;

	// Steal oldest active voice
	if (oldest_active >= 0)
	{
		fm_key_off(oldest_active);
		fm_voices[oldest_active].active = false;
		return oldest_active;
	}

	return 0;
}

// ---------------------------------------------------------------------------
// PSG
// ---------------------------------------------------------------------------

void PreviewSynth::psg_write(uint8_t data)
{
	if (!sn76496)
		return;
	sn76496->write(0, data);
}

void PreviewSynth::load_psg(const uint8_t *data, int len)
{
	psg_envelope.assign(data, data + len);
	psg_envelope_loaded = true;
}

uint16_t PreviewSynth::calc_psg_pitch(uint8_t midi_note)
{
	int note = static_cast<int>(midi_note) - 12; // MIDI 12 = C0
	if (note < 0)
		note = 0;
	if (note > 95)
		note = 95;

	uint8_t octave = note / 12;
	uint8_t semitone = note % 12;
	uint16_t freq = psg_freqtab[semitone];
	freq >>= octave;
	return freq & 0x3ff;
}

void PreviewSynth::psg_set_pitch(int ch, uint8_t midi_note)
{
	uint16_t freq = calc_psg_pitch(midi_note);
	// Latch + low 4 bits
	psg_write(0x80 | (ch << 5) | (freq & 0x0f));
	// High 6 bits
	if (ch < 3)
		psg_write((freq >> 4) & 0x3f);
}

void PreviewSynth::psg_set_volume(int ch, uint8_t vol)
{
	// vol 0 = loudest, 15 = silent
	psg_write(0x90 | (ch << 5) | (vol & 0x0f));
}

int PreviewSynth::psg_allocate_voice()
{
	for (int i = 0; i < 3; i++)
	{
		if (!psg_voices[i].active)
			return i;
	}
	// Steal channel 0
	psg_set_volume(0, 15);
	psg_voices[0].active = false;
	return 0;
}

void PreviewSynth::psg_update_envelopes()
{
	if (!psg_envelope_loaded || psg_envelope.empty())
		return;

	for (int ch = 0; ch < 3; ch++)
	{
		auto &v = psg_voices[ch];
		if (!v.active && !v.env_keyoff)
			continue;

		if (v.env_pos < 0 || static_cast<size_t>(v.env_pos) >= psg_envelope.size())
		{
			psg_set_volume(ch, 15); // silent
			v.active = false;
			continue;
		}

		if (v.env_delay > 0)
		{
			v.env_delay--;
			continue;
		}

		uint8_t byte = psg_envelope[v.env_pos];

		if (byte == 0x00)
		{
			// End
			psg_set_volume(ch, 15);
			v.active = false;
			continue;
		}
		else if (byte == 0x01)
		{
			// Sustain point
			if (v.env_keyoff)
			{
				v.env_pos++;
				v.env_keyoff = false;
			}
			// else: stay here until key off
			continue;
		}
		else if (byte == 0x02)
		{
			// Loop: next byte = target position
			if (static_cast<size_t>(v.env_pos + 1) < psg_envelope.size())
				v.env_pos = psg_envelope[v.env_pos + 1];
			else
				v.env_pos = 0;
			continue;
		}
		else
		{
			// Volume + delay: high nibble = frames-1, low nibble = volume
			uint8_t vol = byte & 0x0f;
			uint8_t frames = (byte >> 4) & 0x0f;
			v.env_vol = vol;
			v.env_delay = frames;
			psg_set_volume(ch, vol);
			v.env_pos++;
		}
	}
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void PreviewSynth::set_mode(int m)
{
	if (m != mode)
	{
		all_notes_off();
		mode = m;
	}
}

void PreviewSynth::note_on(uint8_t midi_note, uint8_t velocity)
{
	if (!initialized)
		return;

	if (mode == 0 && fm_instrument_loaded)
	{
		// FM mode
		// Check if this note is already playing
		for (int i = 0; i < 6; i++)
		{
			if (fm_voices[i].active && fm_voices[i].midi_note == midi_note)
			{
				fm_key_off(i);
				fm_voices[i].active = false;
				break;
			}
		}

		int ch = fm_allocate_voice();
		fm_load_instrument(ch);
		fm_voices[ch].active = true;
		fm_voices[ch].midi_note = midi_note;
		fm_voices[ch].age = ++age_counter;

		fm_set_pitch(ch, midi_note);
		fm_set_volume(ch, velocity);
		fm_key_on(ch);
	}
	else if (mode == 1 && psg_envelope_loaded)
	{
		// PSG mode
		for (int i = 0; i < 3; i++)
		{
			if (psg_voices[i].active && psg_voices[i].midi_note == midi_note)
			{
				psg_set_volume(i, 15);
				psg_voices[i].active = false;
				break;
			}
		}

		int ch = psg_allocate_voice();
		psg_voices[ch].active = true;
		psg_voices[ch].midi_note = midi_note;
		psg_voices[ch].env_pos = 0;
		psg_voices[ch].env_delay = 0;
		psg_voices[ch].env_keyoff = false;
		psg_voices[ch].env_vol = 0;

		psg_set_pitch(ch, midi_note);
		psg_set_volume(ch, 0); // start audible, envelope takes over
	}
}

void PreviewSynth::note_off(uint8_t midi_note)
{
	if (!initialized)
		return;

	if (mode == 0)
	{
		for (int i = 0; i < 6; i++)
		{
			if (fm_voices[i].active && fm_voices[i].midi_note == midi_note)
			{
				fm_key_off(i);
				fm_voices[i].active = false;
				break;
			}
		}
	}
	else if (mode == 1)
	{
		for (int i = 0; i < 3; i++)
		{
			if (psg_voices[i].active && psg_voices[i].midi_note == midi_note)
			{
				psg_voices[i].env_keyoff = true;
				break;
			}
		}
	}
}

void PreviewSynth::all_notes_off()
{
	if (!initialized)
		return;

	for (int i = 0; i < 6; i++)
	{
		if (fm_voices[i].active)
			fm_key_off(i);
		fm_voices[i].active = false;
	}

	for (int i = 0; i < 3; i++)
	{
		psg_set_volume(i, 15);
		psg_voices[i].active = false;
		psg_voices[i].env_keyoff = false;
	}
}

bool PreviewSynth::is_active() const
{
	if (!initialized)
		return false;

	for (int i = 0; i < 6; i++)
		if (fm_voices[i].active)
			return true;

	for (int i = 0; i < 3; i++)
		if (psg_voices[i].active || psg_voices[i].env_keyoff)
			return true;

	return false;
}

void PreviewSynth::render(WAVE_32BS *output, int frames)
{
	if (!initialized || !output || frames <= 0)
		return;

	// Advance PSG envelopes using modular arithmetic
	uint32_t remaining = static_cast<uint32_t>(frames);
	while (remaining > 0)
	{
		uint32_t until_tick = psg_tick_period - psg_tick_counter;
		if (until_tick <= remaining)
		{
			remaining -= until_tick;
			psg_tick_counter = 0;
			psg_update_envelopes();
		}
		else
		{
			psg_tick_counter += remaining;
			break;
		}
	}

	// Render chip audio (additive)
	WAVE_32BS tmp;
	for (int i = 0; i < frames; i++)
	{
		tmp.L = 0;
		tmp.R = 0;
		ym2612->get_sample(&tmp, 1);
		output[i].L += tmp.L;
		output[i].R += tmp.R;

		tmp.L = 0;
		tmp.R = 0;
		sn76496->get_sample(&tmp, 1);
		output[i].L += tmp.L;
		output[i].R += tmp.R;
	}
}
