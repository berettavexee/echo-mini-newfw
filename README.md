# ECHO MINI new firmware

[![check](https://github.com/berettavexee/echo-mini-newfw/actions/workflows/check.yml/badge.svg)](https://github.com/berettavexee/echo-mini-newfw/actions/workflows/check.yml)

**An alternative firmware for the SNOWSKY ECHO MINI music player**, written from scratch.
It plays more formats than the stock firmware — Opus, Ogg Vorbis, Musepack, ALAC and more —
with gapless playback and ReplayGain.

![The play screen](docs/manual/play-screen.png)

## What it does

- **Formats**: WAV, AIFF, FLAC, MP3, Ogg Vorbis, Opus, AAC and ALAC (M4A), Musepack;
  any sample rate from 8 to 192 kHz, played as it is.
- **Gapless playback** and **ReplayGain** (album, track or auto, with a preamp).
- **Folder browser** with long names, on the internal memory and the microSD card (FAT32 or
  exFAT). Play a whole folder tree, in order or shuffled.
- **Settings**: brightness, screen timeout, sleep timer, power off when idle, start-up behaviour.
- Resume where you stopped, seek, clock, USB disk mode, low-battery warning, charging screen.
- **Not there (yet)**: equaliser, music library by artist or album (folders only), lyrics.

## Before you install

- This is **unofficial**: install at your own risk. Keep a stock image on the player and
  learn the recovery mode first (see the manual).
- It is made and tested for the **8 GB model** only (the 512 MB model is not supported).
- The interface is plain. The project began as a test bench for codecs, gapless playback and
  ReplayGain, which could not be added by patching the stock firmware. For a more polished
  custom firmware, look at [RECHO](https://recho.atzkey.workers.dev/).

## Getting started

1. Download `NEWFW096.IMG` from the [latest release](https://github.com/berettavexee/echo-mini-newfw/releases/latest).
2. Follow [Installing](MANUAL.md#installing) in the manual.
3. The [manual](MANUAL.md) shows the keys of each screen.

## How it is made

The firmware runs on the main core of the player's chip (Rockchip RKNanoD), with our own
audio decoders on its second core, the HiFi core ("core B"). It was built by studying the
official firmware V3.9.0, and contains no code from it, from the Rockchip SDK, from ReChord
or from RECHO. The full study, in French, is in [`ETUDE.md`](ETUDE.md).

## Supported formats and core B load

Our decoders run on the HiFi core (core B), whose clock follows the load in steps of 50, 57, 67,
80, 100, 133 and 200 MHz (the step that keeps the load near 75 %). Measured on the device from
the logs (versions 0.73 to 0.93), whole tracks, screen off; since 0.85, parts of the FLAC, MP3,
Vorbis, AAC and Musepack decoders run on the chip's accelerators (`docs/HIFIACC.md`, `docs/MP3ACC.md`):

| Format | Material measured | Core B, mean (clock step) | Decoder |
|---|---|---|---|
| WAV, AIFF | 16 bits, 44.1 kHz | 1 MHz (50 MHz) | ours (`codec_pcm.c`) |
| AIFF | 24 bits, 48 kHz | 8 MHz (50 MHz) | ours |
| AIFF | 24 bits, 96 kHz | 16 MHz (50 MHz) | ours |
| FLAC | 16 bits, 44.1 kHz (640-1,050 kb/s) | 9-12 MHz (50 MHz) | dr_flac, LPC prediction on HIFIACC (0.85) |
| MP3 | 130-320 kb/s, 44.1 kHz | 20-25 MHz (50 MHz) | PacketVideo (AOSP), synthesis on SYNTH (0.87) |
| Ogg Vorbis | 110-200 kb/s, 44.1 kHz | 29-36 MHz (50 MHz) | Tremor (lowmem), IMDCT on HIFIACC's FFT (0.88) |
| Opus | 130-145 kb/s, 48 kHz | 50-55 MHz (67-80 MHz) | libopus 1.5.2 |
| AAC-LC (M4A) | ~125 kb/s, 44.1 kHz (iTunes) | 23 MHz (50 MHz) | PacketVideo (AOSP), IMDCT's FFTs on HIFIACC (0.92) |
| ALAC (M4A) | 16 bits, 44.1 kHz, mono | 11-13 MHz (50 MHz) | ours, derived from Apple's |
| ALAC (M4A) | 16 bits, 44.1 kHz, stereo | 21-25 MHz (50 MHz) | ours, derived from Apple's |
| ALAC (M4A) | 24 bits, 96 kHz, stereo | 49-51 MHz (67-80 MHz) | ours, derived from Apple's |
| Musepack | SV7 and SV8, ~180 kb/s, 44.1 kHz | 10-11 MHz (50 MHz) | libmpcdec, synthesis on SYNTH (0.93) |

The load rises for a moment at the start of a track and after a seek (the top step until the
DAC ring is full). Details and the test conditions: `ETUDE.md` (§52 to §61, accelerators §66 to §77). Sample rates from 8 to
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
| `fw/tools/fuzz/` | fuzzing bench for the decoders and file readers (ASan, UBSan, coverage-guided): see `docs/FUZZING.md` |
| `docs/` | `FUZZING.md` (the fuzzing bench and its findings), `HIFIACC.md` (the RKNanoD audio accelerator: registers, sequence, pitfalls), `MP3ACC.md` (the SYNTH and Imdct36 MP3 accelerators), screenshots of the manual |
| `fw/font/` | Fusion Pixel 12 px (SIL OFL 1.1) |
| `third_party/` | libopus 1.5.2 (decoder-only variant), Tremor lowmem (patched), AOSP pvmp3dec and pvaacdec, Apple ALAC decoder, libmpcdec |
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
- **[EchoPod](https://github.com/BasilSkyWalk/echopod)** by BasilSkyWalk: a replacement firmware
  built around a music library made on the player (iPod-style Cover Flow, favourites, M3U
  playlists, Up Next), with Bluetooth output and a USB DAC mode.

This project is independent from all three and shares no code with them (but I will be happy to share).

## Licenses

Our code is under the [MIT License](LICENSE). The third-party code and the font it includes keep
their own licenses (BSD-3-Clause, Apache-2.0, public domain / MIT-0, SIL OFL 1.1): see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md), which lists them with their copyright holders
and where to find each license text.
