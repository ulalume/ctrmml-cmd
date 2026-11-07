#include "playback.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "exception_text.h"
#include "song.h"
#include "song_blob.h"

namespace
{
	std::shared_ptr<Song> read_song(const uint8_t *data, size_t size)
	{
		if (!data)
			throw std::invalid_argument("song data is truncated");
		auto song = std::make_shared<Song>();
		deserialize_song(data, size, *song);
		return song;
	}
}

bool Playback::init(uint32_t rate)
{
	sample_rate = rate;
	try
	{
		error.reserve(256);
	}
	catch (...)
	{
		return false;
	}
	return true;
}

bool Playback::load(std::shared_ptr<Song> song, uint32_t start_tick)
{
	renderer.reset();
	error.clear();
	try
	{
		renderer = std::make_unique<VgmAudioRenderer>(std::move(song), start_tick, false);
		renderer->set_ym2612_chip_type(chip_type);
		renderer->setup_stream(sample_rate);
		dc_blocker.reset();
		return true;
	}
	catch (...)
	{
		renderer.reset();
		set_exception_error();
		return false;
	}
}

bool Playback::load(const uint8_t *data, size_t size, uint32_t start_tick)
{
	renderer.reset();
	std::shared_ptr<Song> song;
	try
	{
		song = read_song(data, size);
	}
	catch (...)
	{
		set_exception_error();
		return false;
	}
	return load(std::move(song), start_tick);
}

bool Playback::relink(std::shared_ptr<Song> song)
{
	error.clear();
	if (!is_playing() || !renderer->get_driver())
		return true;
	try
	{
		renderer->relink_song(std::move(song), renderer->get_driver()->get_player_ticks());
		return true;
	}
	catch (...)
	{
		set_exception_error();
	}
	renderer.reset();
	return false;
}

bool Playback::relink(const uint8_t *data, size_t size)
{
	error.clear();
	if (!is_playing())
		return true;
	std::shared_ptr<Song> song;
	try
	{
		song = read_song(data, size);
	}
	catch (...)
	{
		set_exception_error();
		return false;
	}
	return relink(std::move(song));
}

void Playback::stop()
{
	renderer.reset();
}

bool Playback::is_playing() const
{
	return renderer && !renderer->is_finished();
}

uint32_t Playback::ticks() const
{
	if (!renderer)
		return 0;
	auto &driver = renderer->get_driver();
	return driver ? driver->get_player_ticks() : 0;
}

void Playback::set_mute_mask(int chip_id, uint32_t mask)
{
	if (renderer)
		renderer->set_mute_mask(chip_id, mask);
}

void Playback::set_ym2612_chip_type(Ym2612ChipType type)
{
	chip_type = type;
	if (renderer)
		renderer->set_ym2612_chip_type(chip_type);
	if (preview_synth)
		preview_synth->set_ym2612_chip_type(chip_type);
}

void Playback::set_stop_at_pcm_end(bool enabled)
{
	if (renderer)
		renderer->set_stop_at_pcm_end(enabled);
}

const std::string &Playback::last_error() const
{
	return error;
}

void Playback::preview_init(uint32_t rate)
{
	try
	{
		preview_synth = std::make_unique<PreviewSynth>();
		preview_synth->set_ym2612_chip_type(chip_type);
		preview_synth->init(rate);
	}
	catch (...)
	{
		preview_synth.reset();
	}
	// The output stage is the song's while one plays.
	if (!is_playing())
		dc_blocker.reset();
}

void Playback::preview_deinit()
{
	preview_synth.reset();
}

PreviewSynth *Playback::preview()
{
	return preview_synth.get();
}

void Playback::render(float *left, float *right, uint32_t frames)
{
	const bool has_song = is_playing();
	const bool has_preview = preview_synth && preview_synth->is_active();
	if (!has_song && !has_preview)
	{
		std::fill(left, left + frames, 0.0f);
		std::fill(right, right + frames, 0.0f);
		return;
	}

	std::memset(mix, 0, frames * sizeof(WAVE_32BS));
	if (has_song)
	{
		renderer->get_sample(mix, static_cast<int>(frames));
		if (renderer->is_finished() && !renderer->last_error().empty())
			set_error(renderer->last_error().c_str());
	}
	if (has_preview)
		preview_synth->render(mix, static_cast<int>(frames));

	for (uint32_t i = 0; i < frames; ++i)
		dc_blocker.to_output(mix[i].L, mix[i].R, left[i], right[i]);
}

void Playback::set_error(const char *text)
{
	try
	{
		error = text;
	}
	catch (...)
	{
	}
}

void Playback::set_exception_error()
{
	set_error(current_exception_text("not enough memory to play this song"));
}
