# ECHO MINI new firmware

> The interface is minimalist and rough around the edges; the primary aim is to test codec
> addition, gapless playback and ReplayGain. These are things I couldn’t do by patching the
> official firmware.
> This project should be seen as a test platform that has grown in scope. If you’re looking for
> a more polished experience, take a look at [Recho](https://recho.atzkey.workers.dev/).

A from-scratch firmware for the main core of the SNOWSKY ECHO MINI digital audio player
(Rockchip RKNanoD, 8 GB model), with our own decoders on the HiFi core (core B). It is built
by studying the official firmware V3.9.0; no code of it, of the Rockchip SDK, of ReChord or
of RECHO is included.

Status (version 0.79): playback of WAV, AIFF, FLAC, MP3, Ogg Vorbis, Opus, AAC and ALAC (M4A), gapless,
at any sample rate from 8 to 192 kHz, with
non-blocking sample-rate changes; folder browser with long names (FAT32 internal memory,
FAT32 or exFAT microSD card); resume, ReplayGain (album / track / off), seek on the bar,
play modes, real-time clock, USB mass storage of the internal memory, power saving in
pause, low-battery warning and power off, charging screen. No equaliser.

**New users: see [`MANUAL.md`](MANUAL.md)** for the keys on each screen and how to install.

## Supported formats and core B load

Our decoders run on the HiFi core (core B), whose clock follows the load in steps of 50, 57, 67,
80, 100, 133 and 200 MHz (the step that keeps the load near 75 %). Measured on the device from
the logs (versions 0.73 to 0.79), whole tracks, screen off:

| Format | Material measured | Core B, mean (clock step) | Decoder |
|---|---|---|---|
| WAV, AIFF | 16 bits, 44.1 kHz | 1 MHz (50 MHz) | ours (`codec_pcm.c`) |
| AIFF | 24 bits, 48 kHz | 8 MHz (50 MHz) | ours |
| AIFF | 24 bits, 96 kHz | 16 MHz (50 MHz) | ours |
| FLAC | 16 bits, 44.1 kHz (~1,050 kb/s) | 15 MHz (50 MHz) | dr_flac |
| MP3 | 320 kb/s, 44.1 kHz | 39-41 MHz (57 MHz) | PacketVideo (AOSP) |
| Ogg Vorbis | 160-280 kb/s, 44.1 kHz | 46-50 MHz (67 MHz) | Tremor (lowmem) |
| Opus | 130-145 kb/s, 48 kHz | 50-55 MHz (67-80 MHz) | libopus 1.5.2 |
| AAC-LC (M4A) | ~125 kb/s, 44.1 kHz (iTunes) | 25 MHz (50 MHz) | PacketVideo (AOSP) |
| ALAC (M4A) | 16 bits, 44.1 kHz, mono | 11-13 MHz (50 MHz) | ours, derived from Apple's |
| ALAC (M4A) | 16 bits, 44.1 kHz, stereo | 21-25 MHz (50 MHz) | ours, derived from Apple's |
| ALAC (M4A) | 24 bits, 96 kHz, stereo | 49-51 MHz (67-80 MHz) | ours, derived from Apple's |

The load rises for a moment at the start of a track and after a seek (the top step until the
DAC ring is full). Details and the test conditions: `ETUDE.md` (§52 to §60). Sample rates from 8 to
192 kHz are played as they are (§60).

## Layout

| Path | Content |
|---|---|
| `ETUDE.md` | the study (in French): hardware, boot, cores, codecs, every version and its tests |
| `MANUAL.md` | user manual: screens, keys, messages, formats, installing |
| `ROADMAP.md` | what is done, waiting for a test, to do, ideas, known limitations |
| `fw/src/` | main-core firmware (C) |
| `fw/hifi/` | HiFi-core runtime and codec glue |
| `fw/tools/` | image builder, emulator (`emu_fw.py`, Unicorn), test disk and exFAT image builders, log reader |
| `fw/font/` | Fusion Pixel 12 px (SIL OFL 1.1) |
| `third_party/` | libopus 1.5.2 (decoder-only variant), Tremor lowmem (patched), AOSP pvmp3dec and pvaacdec, Apple ALAC decoder |
| `tools/` | reverse-engineering helpers used for the study (they expect the stock image and `re/rknanofw.py` next to this tree) |

## Build

    cd fw && make

Needs `arm-none-eabi-gcc` and Python 3. Output: `fw/build/HIFIEC70.IMG`, installed from the
device's own firmware menu or the loader's USB mode. See `fw/README.md`.

## Acknowledgments

More ambitious custom firmware projects exist for this player, and are worth a look:
- **[RECHO](https://recho.atzkey.workers.dev/)** by atzkey: a from-scratch firmware for the ECHO
  MINI (gapless playback, improved navigation, files in name order).
- **[ReChord](https://github.com/brnomt/ReChord)**: a from-source custom firmware for the ECHO
  MINI, in progress.

This project is independent from both and shares no code with them (but I will be happy to share).

## Licenses

Our code: see the headers. Third-party code keeps its license: libopus (BSD, `third_party/opus/COPYING`),
Tremor (BSD, `third_party/tremor-lowmem/COPYING`), pvmp3dec (Apache-2.0,
`third_party/pvmp3dec/NOTICE`, `third_party/pvaacdec/NOTICE`), Apple ALAC decoder (Apache-2.0, `third_party/alac/LICENSE`), dr_flac (public domain / MIT-0), Fusion Pixel font (SIL OFL 1.1).
