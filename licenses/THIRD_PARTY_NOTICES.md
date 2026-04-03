# Third-Party Notices

ctrmml-cmd is released under the GPL v2 License. Every third-party component keeps its upstream license; this directory stores the original license texts.

|                                                                         Project URL | License(s)                                        | License File             |
| ----------------------------------------------------------------------------------: | ------------------------------------------------- | ------------------------ |
|                                            [ymfm](https://github.com/aaronsgiles/ymfm) | BSD-3-Clause                                      | `ymfm.LICENSE.txt`       |
| [sn76496.c](https://github.com/ValleyBell/libvgm/blob/7cad7836/emu/cores/sn76496.c) | BSD-3-Clause                                      | `sn76496.c.LICENSE.txt`  |
| [Resampler](https://github.com/ValleyBell/libvgm/blob/7cad7836/emu/Resampler.c) | GPL-2.0 (libvgm)                                  |                          |
|                                     [miniaudio](https://github.com/mackron/miniaudio) | Public Domain / MIT-0 (dual-licensed)             | `miniaudio.LICENSE.txt`  |
|                                        [ctrmml](https://github.com/superctr/ctrmml) | GPL v2 License                                    | `ctrmml.COPYING.txt`     |
|                                         [mmlgui](https://github.com/superctr/mmlgui) | GPL v2 License                                    | `mmlgui.COPYING.txt`     |
|                                        [MDSDRV](https://github.com/superctr/MDSDRV) | Zlib License                                      | `MDSDRV.COPYING.txt`     |

Note: The files in `src/resampler/` (Resampler, EmuStructs, snddef, stdtype, EmuHelper, EmuCores, logging, common_def) originate from [libvgm](https://github.com/ValleyBell/libvgm) by Valley Bell. libvgm uses a per-file license model; these utility files have no explicit license header and are assumed GPL-2.0 consistent with the project's overall licensing.

Additional note: Parts of the cursor-to-song-position behavior in `highlight_tracker.cpp` were adapted with reference to `mmlgui`'s `song_manager.cpp`.
