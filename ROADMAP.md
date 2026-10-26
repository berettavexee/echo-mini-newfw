# Roadmap

State of the ECHO MINI new firmware, kept up to date with each version. The details of every
step (design, measurements, tests) are in `ETUDE.md` (in French), by section (§).

Last update: 2026-10-26, version 0.82.

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
| AIFF and AIFF-C (uncompressed), in the WAV image; tags after the samples read in WAV and AIFF | 0.74 | §53 |
| AAC-LC in M4A: PacketVideo decoder, MP4 read on core A, gapless, exact seek, iTunes tags and ReplayGain | 0.75 | §54-§55 |
| ALAC in M4A (16, 20, 24 bits), on the same MP4 reader; our decoder derived from Apple's (half the load) | 0.76, 0.77 | §56-§57 |
| Sample rates from 8 to 192 kHz played as they are (the stock firmware's 15 rates, two PLLs corrected); upsampling for unusual rates | 0.78, 0.79 | §59-§60 |

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
  once (3 s before) (§46). Validated on the device with the test set, BACK included, and no regression on real FLAC and Ogg files (seek, gapless, large cover art).
- 0.68: charging screen when the USB cable switches the player on, or Power is held with the
  cable in: a battery bar, "Charging" / "Charged" from the charger, screen off after 30 s, any
  key wakes it, PLAY or Power held starts the player, the cable out powers it off (§48).
  Checked on the device, except the way in through Power held with the cable in.
- 0.69: that way in fixed: the power hold and the backlight on again (power_off() had let them
  go: black screen, and Power restarted the whole player) (§48). Validated on the device.
- 0.70: MP3: the PacketVideo ARM assembly (polyphase window, 18-point MDCT, DCTs) ported to
  Thumb-2: 8 % fewer instructions on the QEMU bench, same quality (within 1 LSB); the load on the
  device is to be measured (§49).
- 0.71: no 24 MHz fallback after an install started less than 5 s after boot (the installer loop
  did not clear the 200 MHz attempt marker) (§50).
- 0.72: the wait state of core B's RAMs (GRF inter_con0 bits 14/15) only above 100 MHz: about
  22 % less load at 67 MHz in an A/B test (MP3 49.7 -> 38.8 MHz, Opus without underruns at
  67 MHz) (§51). **Failed** the endurance test (NEWFWST1, 100 <-> 133 MHz every 250 ms, 8 h): 43
  core B faults in 101 min of Vorbis, 1 in MP3, 1 in Opus. Not for use; 0.71 stays the reference.
  Diagnostic images to tell the causes apart: NEWFWST2 (100 MHz fixed, no wait) and NEWFWST3
  (100 <-> 133 MHz every 250 ms, wait always on).
  Results: no fault at 100 MHz fixed without the wait (ST2), nor at 100 <-> 133 MHz with the wait
  always on (ST3), nor at 133 <-> 200 MHz without the wait (ST4): only switching the wait state
  while core B runs caused the faults.
- 0.73: core B's RAMs without the wait state at every level (50-200 MHz, as the stock firmware
  below 300 MHz), never switched; about 22 % less load; a core B fault also logs the clock (§52).
  Checked on the device: no fault, MP3BENCH 38.9 MHz mean at 57 MHz (49.7 before), FLAC about
  15 MHz at 50 MHz, Opus 52 MHz; overnight battery measurement done on 0.80 (see below).
- 0.74: AIFF and AIFF-C (`.aif`, `.aiff`, `.aifc`; uncompressed "NONE"/"twos" big-endian and
  "sowt" little-endian, 8 to 32 bits, mono or stereo) in one core B image with WAV
  (`hifi/codec_pcm.c`); tags from an ID3v2 chunk or NAME/AUTH, also after the samples; WAV
  RIFF INFO or ID3v2 after the samples read too (§53). Bit-exact in the emulator; validated on
  the device with 0.75 (Discovery as AIFF, AIFF-C, 24/48, 24/96, seeks, damaged files).
- 0.75: AAC-LC in M4A (`.m4a`, `.m4b`, `.mp4`): PacketVideo decoder on core B (chosen against
  Helix on a QEMU bench for its licence, §54); the MP4 container read on core A, tables read from
  the file through small windows, frames sent as a packet stream (for ALAC later too); gapless
  (iTunSMPB, edit list), exact seek, iTunes tags and ReplayGain (§55). Checked in the emulator
  against ffmpeg. Validated on the device: an iTunes album gapless, seeks, tags, ALAC refused,
  damaged files; core B at 25 MHz for AAC 125 kb/s (lowest level), 16 MHz for AIFF 24/96.
- 0.76: ALAC in M4A (16, 20 or 24 bits, mono or stereo): Apple's reference decoder on core B,
  on the MP4 reader and packet stream of 0.75; exact seek (packets are independent), tags and
  ReplayGain (§56). Bit-exact against ffmpeg in the emulator (16-bit, 24/96, mono, after a
  seek). Validated on the device (Pink Floyd tracks converted from FLAC): gapless, tags, seeks,
  damaged files; core B at 41 MHz for 16-bit stereo, 108 MHz (200 MHz level) for 24/96.
- 0.77: our own ALAC decoder (`hifi/alacdec.c`, derived from Apple's): hardware clz in the Rice
  decoder, word reads, the predictor unrolled for orders 1 to 8, output in the ring's format;
  bit-exact with Apple's, half its instructions on the QEMU bench (16-bit stereo -50 %, 24/96
  -59 %) (§57). Validated on the device: 21-25 MHz for 16-bit stereo, 49-51 MHz for 24/96.
- 0.78: sample rates the DACs do not take (MP3 MPEG-2/2.5 at 8-24 kHz, 32 kHz files) were played
  at 44.1 kHz, at the wrong speed (15 MP3 at 22.05 kHz in the library): now upsampled on core B
  by 2, 3, 4 or 6 to 44.1-96 kHz with a polyphase filter (-0.1 dB to 0.4 of the input rate,
  images -70 to -77 dB), exact length, gapless kept (§59). Emulator: bit-exact with a model,
  72 dB against soxr.
- 0.79: the DACs take the 15 rates of the stock firmware, 8 to 192 kHz (found in it: ACODEC PLL
  tables and the DAC rate index; its two PLL entries for 11.025/22.05 and 64/128 kHz are wrong on
  this hardware and replaced by proven ones): all checked by ear with a test image (NEWFWRT2).
  Low rates now play as they are (the upsampler stays for unusual rates); the drain before a rate
  change waits for the ring's real duration (a fixed 2 s cut the end of low-rate tracks) (§60).
  Validated on the device: every rate plays its whole file.
- 0.80: Musepack SV7 and SV8 (`.mpc`, `.mp+`, `.mpp`) with libmpcdec (BSD) in fixed point on
  core B (11.8 M instructions per second of audio on the QEMU bench, less than MP3); APEv2 tags
  and ReplayGain; exact seek; the input protocol can now go back (SV8's seek table at the end)
  (§61). Bit-exact with the bench in the emulator. On the device: 28-29 MHz mean on core B (45.7 MHz at
  most), seek 0.05-0.15 s in SV7, 0.06 s in SV8; damaged files handled as in the emulator. Also:
  a wrong "queued behind" log value after a slow refusal is fixed.
- 0.81: Vorbis hardened from micro-vorbis' list of Tremor lowmem defects and libvorbis' checks
  (written here, not copied): the 18 floor 0 files of the library (libvorbis 2000 betas) made a
  core B fault when refused, now refused cleanly; checks in the codebook, floor 1 and residue
  setup and decoding against malformed streams; core B's malloc no longer wraps on huge sizes;
  blocks over 4096 samples refused. A fuzzing bench for the core B Ogg code (`fw/tools/fuzz/`,
  `docs/FUZZING.md`): Ogg and Opus without a crash; Vorbis from 5,059 crashes in 25 minutes
  (0.80) to none in 433,945 runs. Output bit-exact with 0.80 on 26 whole tracks and in the
  emulator (§62).
- 0.82: the fuzzing bench extended to MP3, AAC, the MP4 container and the tags (§63): our core A
  readers (`mp4.c`, `tags.c`) clean; eight defects fixed in the PacketVideo decoders and in how
  we feed pvaacdec, two of them out-of-bounds writes (AAC TNS and pulses). Final runs without a
  crash; output bit-exact on the library's 34 M4A files and 16 MP3 files, and in the emulator.

## To do

New formats, in this order (agreed 2026-10-24):

1. ~~AIFF~~: done in 0.74.
2. ~~AAC~~ in M4A: done in 0.75 (AAC-LC; HE-AAC not played).
3. ~~ALAC~~ in M4A: done in 0.76.

Other items:

- Study esphome-libs' micro-mp3 (a fork of the OpenCore MP3 decoder, as our pvmp3dec) and
  micro-flac (Apache-2.0): their lists of fixes and hardening, as done for micro-vorbis (§62).
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
  - hardware acceleration of FLAC and MP3, as in the stock firmware: its MP3 module uses the
    IMDCT36 and SYNTH accelerators of the chip (TRM chapters 27-28, ETUDE §18, about 31 MHz of
    the HiFi core), while our MP3 decoder (PacketVideo) does it all in software (about 50 MHz
    on average on the device); whether the stock FLAC module uses any hardware help is not
    known yet.
- Battery percentage: the curve is a generic Li-ion one, to be measured on the device. Overnight
  test (0.61, 2026-10-24): 6 h of MP3 320 kbps from the card, screen off, 4,184 to 3,710 mV (about
  70 mV/h after the first hour). Overnight test (0.80, 2026-10-26): 7.9 h from the card (MP3,
  Vorbis, FLAC), screen off, no error, 4,179 to 3,637 mV; over the same voltage range 4 to 8 %
  slower than 0.61 (68.5 against 74.1 mV/h from 4,072 to 3,710 mV). About 10 h of playback
  estimated; a full discharge, down to the 3.3 V cut-off, still to do.
- Backlight: always at full brightness, no PWM setting yet.

## Ideas, not decided

- Further formats, a distant prospect, only if libraries call for them:
  - DSD (DSF, DFF): native DSD through the I2S and the CS43131, or conversion to PCM;
  - APE (Monkey's Audio, SDK now BSD): the higher compression levels may be too heavy for
    core B;
  - WavPack (BSD reference decoder).
  WMA, TTA and MP2 are not considered (rare, or no permissive decoder). Musepack was listed here
  wrongly: libmpcdec is BSD; done in 0.80.
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

- ALAC: the predictor (63 % of our decoder's time, §57) on the HIFIACC's adaptive FIR, as the
  stock firmware does, or in Thumb-2 assembly; not needed for now (16-bit at the lowest step).
- AAC: assembly optimisations, not a priority (core B already at 25 MHz, the lowest level, for
  AAC 125 kb/s, §55): a Cortex-M3 header for the fixed-point operations (smull/smlal; the
  decoder's ARMv5TE variant uses DSP instructions the M3 lacks), then hand-written Thumb-2 for
  the hot functions of the QEMU profile (fft_rx4_long 25 %, trans4m_freq_2_time_fxp_2 14 %,
  inv_long_complex_rot 9 %, §54); no reference assembly exists in AOSP for this decoder.
- Optional codecs built on Helix MP3 and Helix AAC (RealNetworks, RPSL/RCSL licence), a distant
  prospect: about 22 % (MP3) and 12 % (AAC) fewer instructions and 1 to 2 LSB of error instead of
  5 to 7 on the QEMU bench (ETUDE §19.9, §54); PacketVideo stays the default for its licence.

## Known limitations

- A track shorter than 1.1 s is not shown on the play screen (the DAC ring holds 1.1 s).
- HE-AAC (SBR, parametric stereo), AAC in ADTS (`.aac`), multichannel AAC and ALAC, and 32-bit
  ALAC are not played.
- Floating-point WAV and AIFF-C (core B has no FPU), and compressed AIFF-C (u-law, A-law) are not
  played.
- Files of 4 GiB or more are not listed.
- Ogg Vorbis with floor 0 (libvorbis betas of 2000) or with blocks over 4096 samples (no current
  encoder makes them; Tremor lowmem decodes them wrongly) is refused (§62).

## Not planned

- Equaliser.
- The extras of the stock firmware's generic Rockchip SDK base (e-book reader, photos, video,
  dictionary, calculator, FM radio, recording, Bluetooth): mostly absent from this hardware
  (no radio, microphone or Bluetooth as far as known).
- Any dependency on the stock firmware image.
