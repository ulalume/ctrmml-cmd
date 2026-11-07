#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <resampler/EmuStructs.h>

#include "dc_blocker.h"
#include "preview_synth.h"
#include "vgm_audio_renderer.h"

class Song;

//! A song and the preview synth mixed into one output through a DC blocker:
//! the audio of the player module.
class Playback
{
public:
	//! The most frames one render() call takes.
	static constexpr uint32_t kMaxFrames = 4096;

	//! Sets the output rate of later load() calls and makes room for error
	//! text. Returns false if there is no memory for it.
	bool init(uint32_t sample_rate);

	//! Plays `song` from `start_tick`. Returns false with the reason in
	//! last_error().
	bool load(std::shared_ptr<Song> song, uint32_t start_tick);
	//! As load(), for a song from serialize_song(). Null `data` stands for
	//! bytes that did not arrive.
	bool load(const uint8_t *data, size_t size, uint32_t start_tick);
	//! Swaps the playing song for `song` at the current tick, keeping the
	//! sounding notes; does nothing when no song plays. Returns false with the
	//! reason in last_error(), and stops, if the swap fails.
	bool relink(std::shared_ptr<Song> song);
	//! As relink(), for a song from serialize_song(); not read when no song
	//! plays.
	bool relink(const uint8_t *data, size_t size);
	void stop();
	bool is_playing() const;
	uint32_t ticks() const;
	void set_mute_mask(int chip_id, uint32_t mask);
	void set_ym2612_chip_type(Ym2612ChipType chip_type);
	void set_stop_at_pcm_end(bool enabled);
	//! The last load, relink or playback error; empty if none.
	const std::string &last_error() const;

	//! Starts a new preview synth at `sample_rate`.
	void preview_init(uint32_t sample_rate);
	void preview_deinit();
	//! The preview synth; null before preview_init().
	PreviewSynth *preview();

	//! Renders `frames` (1 to kMaxFrames) of the song and the preview, mixed;
	//! silence when neither plays.
	void render(float *left, float *right, uint32_t frames);

private:
	void set_error(const char *text);
	//! Records the text of the exception being handled.
	void set_exception_error();

	uint32_t sample_rate = 44100;
	Ym2612ChipType chip_type = Ym2612ChipType::Ym2612;
	DcBlocker dc_blocker;
	std::unique_ptr<VgmAudioRenderer> renderer;
	std::unique_ptr<PreviewSynth> preview_synth;
	std::string error;
	WAVE_32BS mix[kMaxFrames];
};
