# ECHO MINI new firmware

A from-scratch firmware for the main core of the SNOWSKY ECHO MINI digital audio player
(Rockchip RKNanoD, 8 GB model), with our own decoders on the HiFi core (core B). It is built
by studying the official firmware V3.9.0; no code of it, of the Rockchip SDK, of ReChord or
of RECHO is included.

Status (version 0.60): playback of WAV, FLAC, MP3, Ogg Vorbis and Opus, gapless, with
non-blocking sample-rate changes; folder browser with long names (FAT32 internal memory,
FAT32 or exFAT microSD card); resume, ReplayGain (album / track / off), seek on the bar,
play modes, real-time clock, USB mass storage of the internal memory, power saving in
pause. No equaliser.

## Layout

| Path | Content |
|---|---|
| `ETUDE.md` | the study (in French): hardware, boot, cores, codecs, every version and its tests |
| `fw/src/` | main-core firmware (C) |
| `fw/hifi/` | HiFi-core runtime and codec glue |
| `fw/tools/` | image builder, emulator (`emu_fw.py`, Unicorn), test disk and exFAT image builders, log reader |
| `fw/font/` | Fusion Pixel 12 px (SIL OFL 1.1) |
| `third_party/` | libopus 1.5.2 (decoder-only variant), Tremor lowmem (patched), AOSP pvmp3dec |
| `tools/` | reverse-engineering helpers used for the study (they expect the stock image and `re/rknanofw.py` next to this tree) |

## Build

    cd fw && make

Needs `arm-none-eabi-gcc` and Python 3. Output: `fw/build/HIFIEC70.IMG`, installed from the
device's own firmware menu or the loader's USB mode. See `fw/README.md`.

## Licenses

Our code: see the headers. Third-party code keeps its license: libopus (BSD, `third_party/opus/COPYING`),
Tremor (BSD, `third_party/tremor-lowmem/COPYING`), pvmp3dec (Apache-2.0,
`third_party/pvmp3dec/NOTICE`), dr_flac (public domain / MIT-0), Fusion Pixel font (SIL OFL 1.1).
