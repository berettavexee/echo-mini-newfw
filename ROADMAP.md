# Roadmap

State of the ECHO MINI new firmware, kept up to date with each version. The details of every
step (design, measurements, tests) are in `ETUDE.md` (in French), by section (§).

Last update: 2026-10-24, version 0.67.

## Done, validated on the device

| Feature | Since | ETUDE |
|---|---|---|
| Playback of WAV, FLAC, MP3, Ogg Vorbis and Opus with our own decoders on the HiFi core | 0.4x | §19-§27 |
| Gapless playback, the next track queued behind the end of the current one | 0.51 | §32 |
| Sample-rate changes between tracks (44.1 / 48 / 96 kHz) without freezing the screen | 0.52 | §33 |
| Folder browser with long names (UTF-8), folders first, natural order | 0.4x | §25 |
| Internal memory (FAT32) and microSD card (FAT32 or exFAT), card insertion and removal | 0.58 | §36-§38 |
| Play queue (folder, or everything under a folder), modes: in order, repeat all, repeat one, shuffle | 0.4x | §25 |
| Resume after power off (folder, track, position, volume, modes), also on the card | 0.4x, 0.58 | §25, §38 |
| ReplayGain album / track / off (tags of FLAC, Vorbis, Opus R128, MP3 ID3v2), track gain shown on yellow | 0.55, 0.59 | §35 |
| Seek on the bar (VOL+ and VOL- together) | 0.50 | §31 |
| Real-time clock and its setting screen | 0.48 | §28 |
| USB mass storage of the internal memory (read only or read/write) | 0.4x | — |
| Power saving in pause (parked track, clocks on the crystal, power off after 10 min) | 0.49 | §30 |
| Reliable keys (loose contacts filtered), volume on release, end of the queue | 0.53, 0.54 | §34 |
| Flicker-free drawing | 0.60 | §39 |
| Firmware installer from the main menu | 0.1x | §11 |

## Done, waiting for a test on the device

- 0.61: the stock codec modules and the "Audio tests" page removed (§40).
- 0.62: quality pass: dead code, duplicates, the repository builds on its own (§41).
- 0.63: user interface split into modules; key and plug events (headphone jacks, USB cable),
  logged only for now (§42).
- 0.64: low battery: warning at 3,400 mV (banner, beeps mixed into the music, a reminder every
  5 min), power off after 30 s under 3,300 mV (two low beeps, paused, resume state saved),
  refusal to start under 3,350 mV (§43). Needs a real discharge on the device; 0.64 installed,
  first checks fine (formats, card, album ReplayGain, seek).
- 0.65: empty files greyed and left out of the queue, damaged files (5 s without audio while
  reading on) skipped, a "Cannot play" banner with the reason when nothing can be played (§44).
- 0.66: the microSD card set up again after the USB, installer and system screens (a card swapped
  meanwhile showed as an empty folder); a card read error shows "SD card (not readable)" (§45).
- 0.67: damaged files, after a test set on the device (`work/damage.py`): BACK passes a track
  that does not open backwards, FLAC skips a damaged frame and stops at the STREAMINFO length,
  Vorbis stops at its last page, an input with no Ogg page in its first 64 KiB is refused at
  once (3 s before) (§46). Validated on the device with the test set, except BACK (not tried).

## To do

New formats, in this order (agreed 2026-10-24):

1. **AIFF**: uncompressed PCM like WAV (big-endian samples, `FORM`/`COMM`/`SSND` chunks), from
   our WAV module.
2. **AAC** in M4A: the PacketVideo/AOSP AAC decoder (Apache-2.0, the family of our MP3 decoder),
   plus an MP4 container reader (sample tables for reading and seeking, iTunes tags and
   ReplayGain, encoder delay and padding for gapless playback).
3. **ALAC** in M4A: Apple's reference decoder (Apache-2.0), on the MP4 reader of step 2.

Other items:

- Commit and push 0.61 to 0.67.
- Actions on the plug events (to be proposed and agreed first):
  - pause when the headphones are unplugged;
  - a USB screen to mount the internal memory or the card, read only or read/write.
- USB access to the microSD card (step S5 of §36).
- USB DAC mode, as in the stock firmware ("USB Connection Options": data copying or "USB Audio
  Decoding", with its own "UAC Volume"): the player as a USB Audio Class sound card for a
  computer or a phone. Needs an isochronous OUT endpoint in our USB driver (bulk only today),
  UAC descriptors and sample rates, a rate feedback (asynchronous) or an adaptive clock, the
  PCM straight into the DAC ring (core A only). Its choice fits the USB screen above.
- A diagnostics, telemetry and remote-control module over the USB link with the PC, to automate
  the tests on the device: read the log and the state live (playback, buffers, core B load,
  battery, keys), send key events and commands (play a file, seek, change a setting), and run
  scripted test sequences from the PC (a vendor USB interface or a second function beside the
  mass storage, to be designed).
- Key lock: a special key combination locks and unlocks the keys, against accidental presses
  in a pocket or a bag (the combination, what stays active when locked and what the screen
  shows to be proposed and agreed first; the key events of keys.c make it a filter in one
  place).
- The play queue holds 400 tracks: PLAY held on "SD card" only takes the first folders.
- Output and DAC settings (CS43131 registers, little code each):
  - headphone gain low / high (low suits sensitive in-ear monitors);
  - volume limiter (an adjustable ceiling);
  - digital filter of the DAC (the stock firmware offers fast or slow roll-off, minimum or
    linear phase, and no oversampling);
  - left/right balance (the stock firmware corrects each DAC, ETUDE §3.5).
- Music features of the stock firmware:
  - media library: all songs, artists, albums, genres, from an index of the tags kept on the
    internal memory, refreshed on demand or after a USB session;
  - favourites: add / remove a track, a list of them;
  - lyrics: synchronised `.lrc` files shown on the play screen;
  - folder jumping: at the end of a folder, go on with the next one;
  - key layouts: modes, swapped volume keys, volume on short or double press.
- Settings and system:
  - a settings screen: screen timeout or always on, brightness, sleep timer, power off after a
    pause (10 min today), default play mode;
  - settings kept in a file of their own, not only in the resume state;
  - an information screen: version, supported formats, free space on the internal memory and
    the card;
  - delete a file from the browser (needs writing to FAT32 and exFAT: read only today);
  - format the internal memory or the card, reset the settings;
  - interface languages besides English (the font already covers Latin, Greek, Cyrillic and
    CJK);
  - colour themes (minor);
  - daylight saving time: the change between summer and winter time (the real-time clock keeps
    local time today, set by hand on its screen, §28).
- To check on the device rather than add:
  - the 4.4 mm balanced output: the plug is detected, nothing is specific to it; check by ear,
    and whether the stock firmware sets the DACs differently for it (mode, gain);
  - charging while off: what the device shows and does when the cable is plugged in;
  - hardware acceleration of FLAC and MP3, as in the stock firmware: its MP3 module uses the
    IMDCT36 and SYNTH accelerators of the chip (TRM chapters 27-28, ETUDE §18, about 31 MHz of
    the HiFi core), while our MP3 decoder (PacketVideo) does it all in software (about 50 MHz
    on average on the device); whether the stock FLAC module uses any hardware help is not
    known yet.
- Battery percentage: the curve is a generic Li-ion one, to be measured on the device. Overnight
  test (0.61, 2026-10-24): 6 h of MP3 320 kbps from the card, screen off, 4,184 to 3,710 mV (about
  70 mV/h after the first hour); about 9-10 h of playback estimated, a full discharge still to do.
- Backlight: always at full brightness, no PWM setting yet.

## Ideas, not decided

- Further formats, a distant prospect, only if libraries call for them:
  - DSD (DSF, DFF): native DSD through the I2S and the CS43131, or conversion to PCM;
  - APE (Monkey's Audio, SDK now BSD): the higher compression levels may be too heavy for
    core B;
  - WavPack (BSD reference decoder).
  WMA, Musepack, TTA and MP2 are not considered (rare, or no permissive decoder).
- Tracker modules (Amiga and PC), a distant prospect:
  - MOD, then S3M, with a small permissive player (micromod, BSD); IT (libxmp-lite, MIT) only
    with samples read from the card through a cache;
  - mixing is light; the limit is memory: the samples must stay resident (about 170 KiB free
    in core B's data today; 300-350 KiB with a smaller DAC ring, the input/output rings and
    the PMU-SRAM, if core B can read the SYSRAM: to be checked in the TRM first);
  - enough for most MODs, the small S3Ms, few ITs.
- Album covers (skipped today).
- M3U playlists.
- ReplayGain preamp (a setting of the settings screen).
- A spoken "battery low" instead of the beeps: a short recording in the image (15-30 KiB), made
  with a speech synthesiser whose licence lets us ship the sound.

## Known limitations

- A track shorter than 1.1 s is not shown on the play screen (the DAC ring holds 1.1 s).
- M4A (AAC, ALAC) and AIFF are not supported yet (see "To do"); AAC relied on a stock module,
  removed in 0.61.
- Files of 4 GiB or more are not listed.

## Not planned

- Equaliser.
- The extras of the stock firmware's generic Rockchip SDK base (e-book reader, photos, video,
  dictionary, calculator, FM radio, recording, Bluetooth): mostly absent from this hardware
  (no radio, microphone or Bluetooth as far as known).
- Any dependency on the stock firmware image.
