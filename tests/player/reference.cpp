// The native reference for tests/player/parity.mjs: runs a script of player
// calls on the Playback class, with songs compiled here instead of packed by
// the main wasm module, and prints what the player module must print for it.
//
//   player-reference <script>
//
// Script lines, and the lines they print:
//   reset <rate>                  a new player at <rate>
//   compile <file.mml>            "compile <rc>"; the song for load and relink
//   load <tick>                   "load <rc> <error>"
//   relink                        "relink <rc> <error>"
//   stop
//   play <max blocks>             renders 128-frame blocks while the song plays;
//                                 "play <blocks> <hash> <playing> <ticks> <error>"
//   render <frames> <count>       "render <hash> <playing> <ticks> <error>"
//   chip <type>, mute <chip> <mask>, pcm_end <0|1>
//   preview_init <rate>, preview_deinit, preview_fm <hex>, preview_psg <hex>,
//   preview_mode <mode>, note_on <note> <velocity>,
//   note_on_attenuation <note> <attenuation>, note_off <note>, all_notes_off,
//   op_mask <mask>, mono <0|1>
// <rc> is 0 or 1 as the player returns it. <hash> is FNV-1a over the 32-bit
// words of each rendered block, left then right channel, followed by its
// playing state and tick count.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "mml_compile.h"
#include "playback.h"

namespace
{
	constexpr uint32_t kBlock = 128;

	struct Hash
	{
		uint32_t value = 2166136261u;

		void add(uint32_t word)
		{
			value = (value ^ word) * 16777619u;
		}
	};

	std::unique_ptr<Playback> g_playback;
	std::shared_ptr<Song> g_song;
	float g_left[Playback::kMaxFrames];
	float g_right[Playback::kMaxFrames];

	void render_block(uint32_t frames, Hash &hash)
	{
		g_playback->render(g_left, g_right, frames);
		uint32_t word;
		for (const float *channel : {g_left, g_right})
		{
			for (uint32_t i = 0; i < frames; ++i)
			{
				std::memcpy(&word, &channel[i], sizeof(word));
				hash.add(word);
			}
		}
		hash.add(g_playback->is_playing() ? 1 : 0);
		hash.add(g_playback->ticks());
	}

	std::string state(const Hash &hash)
	{
		char text[9];
		std::snprintf(text, sizeof(text), "%08x", hash.value);
		return std::string(text) + ' ' + (g_playback->is_playing() ? "1" : "0") + ' ' +
				std::to_string(g_playback->ticks()) + ' ' + g_playback->last_error();
	}

	std::vector<uint8_t> hex_bytes(const std::string &hex)
	{
		std::vector<uint8_t> bytes;
		for (size_t i = 0; i + 1 < hex.size(); i += 2)
			bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
		return bytes;
	}

	std::string read_text(const std::string &path)
	{
		std::ifstream in(path);
		std::ostringstream text;
		text << in.rdbuf();
		return text.str();
	}

	void run(const std::string &command, std::istringstream &args)
	{
		if (command == "reset")
		{
			uint32_t rate = 0;
			args >> rate;
			g_playback = std::make_unique<Playback>();
			g_playback->init(rate);
			return;
		}
		if (command == "compile")
		{
			std::string path;
			std::getline(args >> std::ws, path);
			CompileResult result = compile_mml_text(read_text(path),
					std::filesystem::path(path).parent_path().string(), "input.mml");
			g_song = result.song;
			std::cout << "compile " << (g_song ? 0 : 1) << '\n';
			return;
		}

		Playback &playback = *g_playback;
		PreviewSynth *preview = playback.preview();
		uint32_t a = 0;
		uint32_t b = 0;
		args >> a >> b;
		if ((command == "load" || command == "relink") && !g_song)
			std::cout << command << " 1 no compiled song\n";
		else if (command == "load")
		{
			const bool ok = playback.load(g_song, a);
			std::cout << "load " << (ok ? 0 : 1) << ' ' << playback.last_error() << '\n';
		}
		else if (command == "relink")
		{
			const bool ok = playback.relink(g_song);
			std::cout << "relink " << (ok ? 0 : 1) << ' ' << playback.last_error() << '\n';
		}
		else if (command == "stop")
			playback.stop();
		else if (command == "play")
		{
			Hash hash;
			uint32_t blocks = 0;
			while (blocks < a && playback.is_playing())
			{
				render_block(kBlock, hash);
				blocks++;
			}
			std::cout << "play " << blocks << ' ' << state(hash) << '\n';
		}
		else if (command == "render")
		{
			Hash hash;
			for (uint32_t i = 0; i < b; ++i)
				render_block(a, hash);
			std::cout << "render " << state(hash) << '\n';
		}
		else if (command == "chip")
			playback.set_ym2612_chip_type(a == 1 ? Ym2612ChipType::Ym3438 : Ym2612ChipType::Ym2612);
		else if (command == "mute")
			playback.set_mute_mask(static_cast<int>(a), b);
		else if (command == "pcm_end")
			playback.set_stop_at_pcm_end(a != 0);
		else if (command == "preview_init")
			playback.preview_init(a);
		else if (command == "preview_deinit")
			playback.preview_deinit();
		else if (!preview)
			return;
		else if (command == "preview_fm" || command == "preview_psg")
		{
			args.clear();
			args.seekg(0);
			std::string name, hex;
			args >> name >> hex;
			const std::vector<uint8_t> data = hex_bytes(hex);
			try
			{
				if (command == "preview_fm")
					preview->load_fm(data.data(), static_cast<int>(data.size()));
				else
					preview->load_psg(data.data(), static_cast<int>(data.size()));
			}
			catch (...)
			{
			}
		}
		else if (command == "preview_mode")
			preview->set_mode(static_cast<int>(a));
		else if (command == "note_on")
			preview->note_on(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
		else if (command == "note_on_attenuation")
			preview->note_on_attenuation(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
		else if (command == "note_off")
			preview->note_off(static_cast<uint8_t>(a));
		else if (command == "all_notes_off")
			preview->all_notes_off();
		else if (command == "op_mask")
			preview->set_fm_op_mask(static_cast<uint8_t>(a));
		else if (command == "mono")
			preview->set_mono(a != 0);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: player-reference <script>\n";
		return 2;
	}
	std::ifstream script(argv[1]);
	if (!script)
	{
		std::cerr << "cannot read " << argv[1] << '\n';
		return 2;
	}
	std::string line;
	while (std::getline(script, line))
	{
		std::istringstream args(line);
		std::string command;
		args >> command;
		if (command.empty())
			continue;
		if (command != "reset" && !g_playback)
		{
			std::cerr << "a script starts with reset\n";
			return 2;
		}
		run(command, args);
	}
	return 0;
}
