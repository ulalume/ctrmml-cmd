// The song player and preview synth of the wasm API as a module of their own,
// for an AudioWorkletProcessor: no Emscripten JavaScript, a fixed memory and
// only WASI imports. Songs come from the main module as serialize_song()
// bytes, copied into the inbox.
//
// Output and error text match ctrmml_cmd_wasm_render_audio() and
// ctrmml_cmd_wasm_get_last_error() for the same calls and block sizes.

#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "dc_blocker.h"
#include "lowpass_filter.h"
#include "preview_synth.h"
#include "song_blob.h"
#include "vgm_audio_renderer.h"

#define PLAYER_EXPORT(name) extern "C" __attribute__((export_name(#name)))

namespace
{
	//! The most frames one player_render() call takes.
	constexpr uint32_t kMaxFrames = 4096;
	//! An inbox up to this size is kept after a song is read from it.
	constexpr size_t kKeptInboxSize = 64 * 1024;

	uint32_t g_sample_rate = 44100;
	Ym2612ChipType g_ym2612_chip_type = Ym2612ChipType::Ym2612;
	DcBlocker g_dc_blocker;
	std::unique_ptr<VgmAudioRenderer> g_renderer;
	std::unique_ptr<PreviewSynth> g_preview;
	std::vector<uint8_t> g_inbox;
	std::string g_last_error;
	WAVE_32BS g_mix[kMaxFrames];
	float g_output[kMaxFrames * 2];

	void set_error(const char *message)
	{
		try
		{
			g_last_error = message;
		}
		catch (...)
		{
		}
	}

	void release_inbox()
	{
		if (g_inbox.capacity() > kKeptInboxSize)
			std::vector<uint8_t>().swap(g_inbox);
	}

	std::shared_ptr<Song> read_inbox(uint32_t size)
	{
		try
		{
			if (size > g_inbox.size())
				throw std::invalid_argument("song data is truncated");
			auto song = std::make_shared<Song>();
			deserialize_song(g_inbox.data(), size, *song);
			release_inbox();
			return song;
		}
		catch (...)
		{
			release_inbox();
			throw;
		}
	}

	//! Records why a load or relink failed.
	void set_load_error(std::exception &e)
	{
		if (dynamic_cast<std::bad_alloc *>(&e))
			set_error("not enough memory to play this song");
		else
			set_error(e.what());
	}
}

//! Sets the output rate of later player_load() and preview_init() calls.
PLAYER_EXPORT(player_init) int player_init(uint32_t sample_rate)
{
	g_sample_rate = sample_rate;
	try
	{
		g_last_error.reserve(256);
	}
	catch (...)
	{
		return 0;
	}
	return 1;
}

//! Where to copy `size` bytes for player_load(), player_relink(),
//! preview_load_fm() or preview_load_psg(); null if there is no room.
PLAYER_EXPORT(player_inbox) uint8_t *player_inbox(uint32_t size)
{
	try
	{
		if (g_inbox.size() < size)
		{
			// Freed first: the old contents are not needed.
			std::vector<uint8_t>().swap(g_inbox);
			g_inbox.resize(size);
		}
		return g_inbox.data();
	}
	catch (...)
	{
		std::vector<uint8_t>().swap(g_inbox);
		return nullptr;
	}
}

//! Starts playing the song in the inbox at `start_tick`. Returns 0, or 1
//! with the reason in player_last_error().
PLAYER_EXPORT(player_load) int player_load(uint32_t size, uint32_t start_tick)
{
	g_renderer.reset();
	g_last_error.clear();
	try
	{
		g_renderer = std::make_unique<VgmAudioRenderer>(read_inbox(size), start_tick, false);
		g_renderer->set_ym2612_chip_type(g_ym2612_chip_type);
		g_renderer->setup_stream(g_sample_rate);
		g_renderer->set_lowpass(true, LowPassFilter::kDefaultCutoffHz);
		g_dc_blocker.reset();
		return 0;
	}
	catch (std::exception &e)
	{
		g_renderer.reset();
		set_load_error(e);
	}
	catch (...)
	{
		g_renderer.reset();
		set_error("unknown exception");
	}
	return 1;
}

//! Swaps the playing song for the one in the inbox at the current tick,
//! keeping the sounding notes. Does nothing more than read the song when
//! nothing plays. Returns 0, or 1 with the reason in player_last_error();
//! playback stops only if the swap itself fails.
PLAYER_EXPORT(player_relink) int player_relink(uint32_t size)
{
	g_last_error.clear();
	std::shared_ptr<Song> song;
	try
	{
		song = read_inbox(size);
	}
	catch (std::exception &e)
	{
		set_load_error(e);
		return 1;
	}
	catch (...)
	{
		set_error("unknown exception");
		return 1;
	}

	if (!g_renderer || g_renderer->is_finished() || !g_renderer->get_driver())
		return 0;
	try
	{
		g_renderer->relink_song(song, g_renderer->get_driver()->get_player_ticks());
		return 0;
	}
	catch (std::exception &e)
	{
		set_load_error(e);
	}
	catch (...)
	{
		set_error("unknown exception");
	}
	g_renderer.reset();
	return 1;
}

PLAYER_EXPORT(player_stop) void player_stop()
{
	g_renderer.reset();
}

PLAYER_EXPORT(player_is_playing) int player_is_playing()
{
	return g_renderer && !g_renderer->is_finished() ? 1 : 0;
}

PLAYER_EXPORT(player_ticks) uint32_t player_ticks()
{
	if (!g_renderer)
		return 0;
	auto &driver = g_renderer->get_driver();
	return driver ? driver->get_player_ticks() : 0;
}

PLAYER_EXPORT(player_set_mute_mask) void player_set_mute_mask(int32_t chip_id, uint32_t mask)
{
	if (g_renderer)
		g_renderer->set_mute_mask(chip_id, mask);
}

PLAYER_EXPORT(player_set_ym2612_chip_type) void player_set_ym2612_chip_type(int chip_type)
{
	g_ym2612_chip_type = chip_type == static_cast<int>(Ym2612ChipType::Ym3438)
			? Ym2612ChipType::Ym3438
			: Ym2612ChipType::Ym2612;
	if (g_renderer)
		g_renderer->set_ym2612_chip_type(g_ym2612_chip_type);
	if (g_preview)
		g_preview->set_ym2612_chip_type(g_ym2612_chip_type);
}

PLAYER_EXPORT(player_set_stop_at_pcm_end) void player_set_stop_at_pcm_end(int enabled)
{
	if (g_renderer)
		g_renderer->set_stop_at_pcm_end(enabled != 0);
}

//! The last load, relink or playback error, NUL-terminated; empty if none.
PLAYER_EXPORT(player_last_error) const char *player_last_error()
{
	return g_last_error.c_str();
}

PLAYER_EXPORT(player_last_error_size) uint32_t player_last_error_size()
{
	return static_cast<uint32_t>(g_last_error.size());
}

//! Renders `frames` (1 to 4096) of the song and the preview, mixed, and
//! returns them planar: `frames` left samples, then `frames` right samples.
//! Silence when neither plays. Null for a frame count out of range.
PLAYER_EXPORT(player_render) float *player_render(uint32_t frames)
{
	if (frames == 0 || frames > kMaxFrames)
		return nullptr;

	const bool has_main = g_renderer && !g_renderer->is_finished();
	const bool has_preview = g_preview && g_preview->is_active();
	if (!has_main && !has_preview)
	{
		std::memset(g_output, 0, frames * 2 * sizeof(float));
		return g_output;
	}

	std::memset(g_mix, 0, frames * sizeof(WAVE_32BS));
	if (has_main)
	{
		g_renderer->get_sample(g_mix, static_cast<int>(frames));
		if (g_renderer->is_finished() && !g_renderer->last_error().empty())
			set_error(g_renderer->last_error().c_str());
	}
	if (has_preview)
		g_preview->render(g_mix, static_cast<int>(frames));

	float *left = g_output;
	float *right = g_output + frames;
	for (uint32_t i = 0; i < frames; ++i)
		g_dc_blocker.to_output(g_mix[i].L, g_mix[i].R, left[i], right[i]);
	return g_output;
}

// ---------------------------------------------------------------------------
// Preview synth, as in the main module
// ---------------------------------------------------------------------------

PLAYER_EXPORT(preview_init) void preview_init(uint32_t sample_rate)
{
	try
	{
		g_preview = std::make_unique<PreviewSynth>();
		g_preview->set_ym2612_chip_type(g_ym2612_chip_type);
		g_preview->set_lowpass(true, LowPassFilter::kDefaultCutoffHz);
		g_preview->init(sample_rate);
	}
	catch (...)
	{
		g_preview.reset();
	}
	g_dc_blocker.reset();
}

PLAYER_EXPORT(preview_deinit) void preview_deinit()
{
	g_preview.reset();
}

PLAYER_EXPORT(preview_load_fm) void preview_load_fm(const uint8_t *data, int len)
{
	if (g_preview)
		g_preview->load_fm(data, len);
}

PLAYER_EXPORT(preview_load_psg) void preview_load_psg(const uint8_t *data, int len)
{
	if (!g_preview)
		return;
	try
	{
		g_preview->load_psg(data, len);
	}
	catch (...)
	{
	}
}

PLAYER_EXPORT(preview_set_mode) void preview_set_mode(int mode)
{
	if (g_preview)
		g_preview->set_mode(mode);
}

PLAYER_EXPORT(preview_note_on) void preview_note_on(uint8_t midi_note, uint8_t velocity)
{
	if (g_preview)
		g_preview->note_on(midi_note, velocity);
}

PLAYER_EXPORT(preview_note_on_attenuation) void preview_note_on_attenuation(uint8_t midi_note, uint8_t tl_attenuation)
{
	if (g_preview)
		g_preview->note_on_attenuation(midi_note, tl_attenuation);
}

PLAYER_EXPORT(preview_note_off) void preview_note_off(uint8_t midi_note)
{
	if (g_preview)
		g_preview->note_off(midi_note);
}

PLAYER_EXPORT(preview_set_fm_op_mask) void preview_set_fm_op_mask(uint8_t mask)
{
	if (g_preview)
		g_preview->set_fm_op_mask(mask);
}

PLAYER_EXPORT(preview_set_mono) void preview_set_mono(int mono)
{
	if (g_preview)
		g_preview->set_mono(mono != 0);
}

PLAYER_EXPORT(preview_all_notes_off) void preview_all_notes_off()
{
	if (g_preview)
		g_preview->all_notes_off();
}
