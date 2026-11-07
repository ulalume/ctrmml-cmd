// The song player and preview synth as a module of their own, for an
// AudioWorkletProcessor: no Emscripten JavaScript, a fixed memory and only
// WASI imports. Songs come from the main module as serialize_song() bytes,
// copied into the inbox.

#include <cstdint>
#include <memory>
#include <new>

#include "playback.h"

#define PLAYER_EXPORT(name) extern "C" __attribute__((export_name(#name)))

namespace
{
	Playback g_playback;
	std::unique_ptr<uint8_t[]> g_inbox;
	uint32_t g_inbox_size = 0;
	float g_output[Playback::kMaxFrames * 2];

	//! The inbox if it holds `size` bytes, otherwise null.
	const uint8_t *inbox_data(uint32_t size)
	{
		return size <= g_inbox_size ? g_inbox.get() : nullptr;
	}
}

//! Sets the output rate of later player_load() calls. Returns 1, or 0 if
//! there is no memory to start.
PLAYER_EXPORT(player_init) int player_init(uint32_t sample_rate)
{
	return g_playback.init(sample_rate) ? 1 : 0;
}

//! Where to copy `size` bytes for player_load(), player_relink(),
//! preview_load_fm() or preview_load_psg(); null if there is no room. The
//! inbox is kept for later calls and only grows.
PLAYER_EXPORT(player_inbox) uint8_t *player_inbox(uint32_t size)
{
	if (size > g_inbox_size)
	{
		// Freed first: the old contents are not needed.
		g_inbox.reset();
		g_inbox.reset(new (std::nothrow) uint8_t[size]);
		g_inbox_size = g_inbox ? size : 0;
	}
	return g_inbox.get();
}

//! Starts playing the song in the inbox at `start_tick`. Returns 0, or 1
//! with the reason in player_last_error().
PLAYER_EXPORT(player_load) int player_load(uint32_t size, uint32_t start_tick)
{
	return g_playback.load(inbox_data(size), size, start_tick) ? 0 : 1;
}

//! Swaps the playing song for the one in the inbox at the current tick,
//! keeping the sounding notes. Does nothing when no song plays. Returns 0,
//! or 1 with the reason in player_last_error(); playback stops only if the
//! swap itself fails.
PLAYER_EXPORT(player_relink) int player_relink(uint32_t size)
{
	return g_playback.relink(inbox_data(size), size) ? 0 : 1;
}

PLAYER_EXPORT(player_stop) void player_stop()
{
	g_playback.stop();
}

PLAYER_EXPORT(player_is_playing) int player_is_playing()
{
	return g_playback.is_playing() ? 1 : 0;
}

PLAYER_EXPORT(player_ticks) uint32_t player_ticks()
{
	return g_playback.ticks();
}

PLAYER_EXPORT(player_set_mute_mask) void player_set_mute_mask(int32_t chip_id, uint32_t mask)
{
	g_playback.set_mute_mask(chip_id, mask);
}

PLAYER_EXPORT(player_set_ym2612_chip_type) void player_set_ym2612_chip_type(int chip_type)
{
	g_playback.set_ym2612_chip_type(chip_type == static_cast<int>(Ym2612ChipType::Ym3438)
			? Ym2612ChipType::Ym3438
			: Ym2612ChipType::Ym2612);
}

PLAYER_EXPORT(player_set_stop_at_pcm_end) void player_set_stop_at_pcm_end(int enabled)
{
	g_playback.set_stop_at_pcm_end(enabled != 0);
}

//! The last load, relink or playback error, NUL-terminated; empty if none.
//! An InputError's text holds its source location in the song.
PLAYER_EXPORT(player_last_error) const char *player_last_error()
{
	return g_playback.last_error().c_str();
}

PLAYER_EXPORT(player_last_error_size) uint32_t player_last_error_size()
{
	return static_cast<uint32_t>(g_playback.last_error().size());
}

//! Renders `frames` (1 to 4096) of the song and the preview, mixed, and
//! returns them planar: `frames` left samples, then `frames` right samples.
//! Silence when neither plays. Null for a frame count out of range.
PLAYER_EXPORT(player_render) float *player_render(uint32_t frames)
{
	if (frames == 0 || frames > Playback::kMaxFrames)
		return nullptr;
	g_playback.render(g_output, g_output + frames, frames);
	return g_output;
}

// ---------------------------------------------------------------------------
// Preview synth
// ---------------------------------------------------------------------------

//! Starts a new preview synth with its own output rate.
PLAYER_EXPORT(preview_init) void preview_init(uint32_t sample_rate)
{
	g_playback.preview_init(sample_rate);
}

PLAYER_EXPORT(preview_deinit) void preview_deinit()
{
	g_playback.preview_deinit();
}

PLAYER_EXPORT(preview_load_fm) void preview_load_fm(const uint8_t *data, int len)
{
	if (PreviewSynth *preview = g_playback.preview())
		preview->load_fm(data, len);
}

PLAYER_EXPORT(preview_load_psg) void preview_load_psg(const uint8_t *data, int len)
{
	PreviewSynth *preview = g_playback.preview();
	if (!preview)
		return;
	try
	{
		preview->load_psg(data, len);
	}
	catch (...)
	{
	}
}

PLAYER_EXPORT(preview_set_mode) void preview_set_mode(int mode)
{
	if (PreviewSynth *preview = g_playback.preview())
		preview->set_mode(mode);
}

PLAYER_EXPORT(preview_note_on) void preview_note_on(uint8_t midi_note, uint8_t velocity)
{
	if (PreviewSynth *preview = g_playback.preview())
		preview->note_on(midi_note, velocity);
}

PLAYER_EXPORT(preview_note_on_attenuation) void preview_note_on_attenuation(uint8_t midi_note, uint8_t tl_attenuation)
{
	if (PreviewSynth *preview = g_playback.preview())
		preview->note_on_attenuation(midi_note, tl_attenuation);
}

PLAYER_EXPORT(preview_note_off) void preview_note_off(uint8_t midi_note)
{
	if (PreviewSynth *preview = g_playback.preview())
		preview->note_off(midi_note);
}

PLAYER_EXPORT(preview_set_fm_op_mask) void preview_set_fm_op_mask(uint8_t mask)
{
	if (PreviewSynth *preview = g_playback.preview())
		preview->set_fm_op_mask(mask);
}

PLAYER_EXPORT(preview_set_mono) void preview_set_mono(int mono)
{
	if (PreviewSynth *preview = g_playback.preview())
		preview->set_mono(mono != 0);
}

PLAYER_EXPORT(preview_all_notes_off) void preview_all_notes_off()
{
	if (PreviewSynth *preview = g_playback.preview())
		preview->all_notes_off();
}
