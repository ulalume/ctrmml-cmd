#include "wav_export.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <player/droplayer.hpp>
#include <player/gymplayer.hpp>
#include <player/playera.hpp>
#include <player/playerbase.hpp>
#include <player/s98player.hpp>
#include <player/vgmplayer.hpp>
#include <utils/MemoryLoader.h>

#include "mml_compile.h"

namespace
{
	const unsigned int kWavBufferFrames = 2048;
	const unsigned int kWavSampleRate = 44100;
	const unsigned int kWavBitDepth = 16;
	const unsigned int kWavChannels = 2;
	const unsigned int kWavFadeSeconds = 8;
	const unsigned int kWavLoops = 2;
	const char *kExtensibleGuidTrailer = "\x00\x00\x00\x00\x10\x00\x80\x00\x00\xAA\x00\x38\x9B\x71";

	void pack_int16le(UINT8 *d, INT16 n)
	{
		d[0] = static_cast<UINT8>(static_cast<UINT16>(n));
		d[1] = static_cast<UINT8>(static_cast<UINT16>(n) >> 8);
	}

	void pack_uint16le(UINT8 *d, UINT16 n)
	{
		d[0] = static_cast<UINT8>(n);
		d[1] = static_cast<UINT8>(n >> 8);
	}

	void pack_int24le(UINT8 *d, INT32 n)
	{
		d[0] = static_cast<UINT8>(n);
		d[1] = static_cast<UINT8>(n >> 8);
		d[2] = static_cast<UINT8>(n >> 16);
	}

	void pack_uint32le(UINT8 *d, UINT32 n)
	{
		d[0] = static_cast<UINT8>(n);
		d[1] = static_cast<UINT8>(n >> 8);
		d[2] = static_cast<UINT8>(n >> 16);
		d[3] = static_cast<UINT8>(n >> 24);
	}

	bool write_wav_header(FILE *f, unsigned int total_frames)
	{
		unsigned int data_size = total_frames * (kWavBitDepth / 8) * kWavChannels;
		UINT8 tmp[4];
		if (fwrite("RIFF", 1, 4, f) != 4)
			return false;
		pack_uint32le(tmp, 4 + (8 + data_size) + (8 + 40));
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		if (fwrite("WAVE", 1, 4, f) != 4)
			return false;
		if (fwrite("fmt ", 1, 4, f) != 4)
			return false;

		pack_uint32le(tmp, 40);
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		pack_uint16le(tmp, 0xFFFE);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint16le(tmp, kWavChannels);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint32le(tmp, kWavSampleRate);
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		pack_uint32le(tmp, kWavSampleRate * kWavChannels * (kWavBitDepth / 8));
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		pack_uint16le(tmp, kWavChannels * (kWavBitDepth / 8));
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint16le(tmp, kWavBitDepth);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint16le(tmp, 22);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint16le(tmp, kWavBitDepth);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		pack_uint32le(tmp, 3);
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		pack_uint16le(tmp, 1);
		if (fwrite(tmp, 1, 2, f) != 2)
			return false;

		if (fwrite(kExtensibleGuidTrailer, 1, 14, f) != 14)
			return false;

		if (fwrite("data", 1, 4, f) != 4)
			return false;

		pack_uint32le(tmp, data_size);
		if (fwrite(tmp, 1, 4, f) != 4)
			return false;

		return true;
	}

	void frames_to_little_endian(UINT8 *data, unsigned int frame_count)
	{
		for (unsigned int i = 0; i < frame_count; ++i)
		{
			if (kWavBitDepth == 16)
			{
				pack_int16le(&data[0], *reinterpret_cast<INT16 *>(&data[0]));
				pack_int16le(&data[2], *reinterpret_cast<INT16 *>(&data[2]));
			}
			else
			{
				pack_int24le(&data[0], *reinterpret_cast<INT32 *>(&data[0]) & 0x00FFFFFF);
				pack_int24le(&data[3], *reinterpret_cast<INT32 *>(&data[3]) & 0x00FFFFFF);
			}
			data += ((kWavBitDepth / 8) * kWavChannels);
		}
	}

	bool write_frames(FILE *f, unsigned int frame_count, UINT8 *data)
	{
		return fwrite(data, (kWavBitDepth / 8) * kWavChannels, frame_count, f) == frame_count;
	}
}

bool export_wav(const std::string &in_path, const std::string &out_path)
{
	auto compile = compile_mml_file(in_path);
	if (!compile.song)
	{
		std::cerr << compile.error << std::endl;
		return false;
	}

	std::vector<uint8_t> data = compile.song->get_platform()->get_export_data(*compile.song.get(), 0);
	if (data.empty())
	{
		std::cerr << "failed to export vgm data\n";
		return false;
	}

	PlayerA player;
	player.RegisterPlayerEngine(new VGMPlayer);
	player.RegisterPlayerEngine(new S98Player);
	player.RegisterPlayerEngine(new DROPlayer);
	player.RegisterPlayerEngine(new GYMPlayer);

	if (player.SetOutputSettings(kWavSampleRate, kWavChannels, kWavBitDepth, kWavBufferFrames))
	{
		std::cerr << "unsupported sample rate / bit depth\n";
		return false;
	}

	PlayerA::Config config = player.GetConfiguration();
	config.masterVol = 0x10000;
	config.loopCount = kWavLoops;
	config.fadeSmpls = kWavSampleRate * kWavFadeSeconds;
	config.endSilenceSmpls = 0;
	config.pbSpeed = 1.0;
	player.SetConfiguration(config);

	FILE *out = fopen(out_path.c_str(), "wb");
	if (!out)
	{
		std::cerr << "unable to open output file\n";
		return false;
	}

	DATA_LOADER *loader = MemoryLoader_Init(reinterpret_cast<const UINT8 *>(data.data()),
																					static_cast<UINT32>(data.size()));
	if (!loader)
	{
		std::cerr << "failed to create memory loader\n";
		fclose(out);
		return false;
	}

	DataLoader_SetPreloadBytes(loader, 0x100);
	if (DataLoader_Load(loader))
	{
		std::cerr << "failed to load vgm data\n";
		MemoryLoader_Deinit(loader);
		fclose(out);
		return false;
	}

	if (player.LoadFile(loader))
	{
		std::cerr << "failed to load vgm data\n";
		MemoryLoader_Deinit(loader);
		fclose(out);
		return false;
	}

	PlayerBase *engine = player.GetPlayer();
	if (engine && engine->GetPlayerType() == FCC_VGM)
	{
		auto *vgmplayer = dynamic_cast<VGMPlayer *>(engine);
		if (vgmplayer)
			player.SetLoopCount(vgmplayer->GetModifiedLoopCount(kWavLoops));
	}

	player.Start();

	unsigned int total_frames = 0;
	if (engine)
		total_frames = engine->Tick2Sample(engine->GetTotalPlayTicks(kWavLoops));
	if (engine && engine->GetLoopTicks())
		total_frames += kWavSampleRate * kWavFadeSeconds;

	if (!write_wav_header(out, total_frames))
	{
		std::cerr << "failed to write wav header\n";
		player.Stop();
		player.UnloadFile();
		MemoryLoader_Deinit(loader);
		fclose(out);
		return false;
	}

	std::vector<UINT8> packed(sizeof(INT32) * kWavChannels * kWavBufferFrames);
	while (total_frames)
	{
		unsigned int cur_frames = (kWavBufferFrames > total_frames) ? total_frames : kWavBufferFrames;
		std::memset(packed.data(), 0, packed.size());
		player.Render(cur_frames * ((kWavBitDepth / 8) * kWavChannels), packed.data());
		frames_to_little_endian(packed.data(), cur_frames);
		if (!write_frames(out, cur_frames, packed.data()))
			break;
		total_frames -= cur_frames;
	}

	player.Stop();
	player.UnloadFile();
	player.UnregisterAllPlayers();
	MemoryLoader_Deinit(loader);
	fclose(out);
	return true;
}
