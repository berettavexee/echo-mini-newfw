# Roadmap

State of the ECHO MINI new firmware, kept up to date with each version. The details of every
step (design, measurements, tests) are in `ETUDE.md` (in French), by section (§).

Last update: 2026-10-29, version 0.95.

## Done, validated on the device

| Feature | Since | ETUDE |
|---|---|---|
| Playback of WAV, FLAC, MP3, Ogg Vorbis and Opus with our own decoders on the HiFi core | 0.4x | §19-§27 |
| Gapless playback, the next track queued behind the end of the current one | 0.51 | §32 |
| Sample-rate changes between tracks without freezing the screen | 0.52 | §33 |
| Folder browser with long names (UTF-8), folders first, natural order | 0.4x | §25 |
| Lists with the position "selected/total" and a scroll bar | 0.90 | — |
| Internal memory (FAT32) and microSD card (FAT32 or exFAT), card insertion and removal | 0.58, 0.66 | §36-§38, §45 |
| Play queue (folder, or everything under a folder), modes: in order, repeat all, repeat one, shuffle | 0.4x | §25 |
| Resume after power off (folder, track, position, volume, modes), also on the card | 0.4x, 0.58 | §25, §38 |
| ReplayGain album / track / off (FLAC, Vorbis, Opus R128, MP3 ID3v2, APEv2, iTunes), track gain shown on yellow | 0.55, 0.59 | §35 |
| Seek on the bar (VOL+ and VOL- together); fast seek in Ogg files | 0.50, 0.89 | §31, §75 |
| Real-time clock and its setting screen | 0.48 | §28 |
| USB mass storage of the internal memory (read only or read/write) | 0.4x | — |
| Power saving in pause (parked track, clocks on the crystal, power off after 10 min) | 0.49 | §30 |
| Reliable keys (loose contacts filtered), volume on release, end of the queue | 0.53, 0.54 | §34 |
| Flicker-free drawing | 0.60 | §39 |
| Firmware installer from the main menu | 0.1x | §11 |
| Low battery: warning, power off, refusal to start; battery gauge from a measured discharge (about 15.5 h of playback) | 0.64, 0.84 | §43, §61, §65 |
| Play queue by folders: PLAY held on a tree or the whole card (4,000 folders), shuffle as a permutation of all the tracks | 0.95 | §79 |
| Empty files greyed, damaged files skipped, a "Cannot play" screen: what went wrong (the reason of a refusal) and what to do | 0.65-0.67, 0.94 | §44, §46, §78 |
| Charging screen (cable in at power on, or Power held with the cable in) | 0.68, 0.69 | §48 |
| Core B's RAMs without the wait state (about 22 % less load) | 0.73 | §51-§52 |
| AIFF and AIFF-C (uncompressed), in the WAV image; tags after the samples read in WAV and AIFF | 0.74 | §53 |
| AAC-LC in M4A: PacketVideo decoder, MP4 read on core A, gapless, exact seek, iTunes tags and ReplayGain | 0.75 | §54-§55 |
| ALAC in M4A (16, 20, 24 bits), on the same MP4 reader; our decoder derived from Apple's (half the load) | 0.76, 0.77 | §56-§57 |
| Sample rates from 8 to 192 kHz played as they are (the stock firmware's 15 rates, two PLLs corrected); upsampling for unusual rates | 0.78, 0.79 | §59-§60 |
| Musepack SV7 and SV8, APEv2 tags, exact seek | 0.80 | §61 |
| Decoders hardened against damaged files; a fuzzing bench (Ogg, Vorbis, Opus, MP3, AAC, MP4, tags, FAT) | 0.81-0.83, 0.91 | §62-§64 |
| The chip's accelerators: FLAC LPC (HIFIACC), MP3 synthesis (SYNTH), Vorbis IMDCT (HIFIACC FFT), AAC IMDCT FFTs (HIFIACC), Musepack synthesis (SYNTH): 28 to 65 % less core B load | 0.85-0.93 | §66-§77 |
| Automatic checks (`make check`, pre-commit hook, GitHub Actions); images reproducible byte for byte | 0.91, 0.92 | — |

## Waiting for a test on the device

Nothing at the moment.

## Version history

One line per version; the measurements are in `ETUDE.md`.

- 0.61-0.63: the stock codec modules removed; quality pass; user interface split into modules,
  key and plug events (§40-§42).
- 0.64: low battery: warning, power off under 3,300 mV, refusal to start under 3,350 mV (§43).
- 0.65-0.67: empty and damaged files; a "Cannot play" banner (§44, §46).
- 0.66: the microSD card set up again after the USB, installer and system screens (§45).
- 0.68-0.69: charging screen (§48).
- 0.70: MP3: PacketVideo's ARM assembly ported to Thumb-2 (8 % fewer instructions) (§49).
- 0.71: no 24 MHz fallback after an early install (§50).
- 0.72: core B's RAM wait state switched with the clock: **failed** the endurance test (core B
  faults); switching the wait state while core B runs was the cause (§51).
- 0.73: core B's RAMs without the wait state at every level, never switched (§52).
- 0.74: AIFF and AIFF-C (§53).
- 0.75: AAC-LC in M4A (§54-§55).
- 0.76-0.77: ALAC in M4A, then our own ALAC decoder (§56-§57).
- 0.78-0.79: unusual sample rates upsampled, then the DACs' 15 rates from 8 to 192 kHz (§59-§60).
- 0.80: Musepack SV7 and SV8 (§61).
- 0.81-0.83: Vorbis, MP3 and AAC hardened; the fuzzing bench (§62-§64, `docs/FUZZING.md`).
- 0.84: battery gauge from the measured discharge; warning at 3,425 mV (§65).
- 0.85-0.86: FLAC LPC on HIFIACC (§66-§67, `docs/HIFIACC.md`).
- 0.87: MP3 synthesis on SYNTH (§68-§71, `docs/MP3ACC.md`).
- 0.88: Vorbis IMDCT on HIFIACC's FFT; underrun counters per track (§73-§74).
- 0.89: fast seek in Ogg files (§75).
- 0.90: list position and scroll bar.
- 0.91: quality pass, `make check`, pre-commit hook; FAT fuzzing target (9 million runs, no
  defect).
- 0.92: AAC IMDCT FFTs on HIFIACC; GitHub Actions; first release (§76).
- 0.93: Musepack synthesis on SYNTH; fix: Musepack files did not open in 0.91 and 0.92 (§77).
- 0.94: expressive error messages: the reason of a refusal from the codecs and the MP4 reader,
  what to do, an `error:` log line (§78).
- 0.95: the play queue by folders, no longer 400 tracks: PLAY held on the whole card (515 tracks,
  35 folders, 60 ms on the device); shuffle as a permutation of all the tracks (§79). The
  emulator about 2.5 times faster (SYNTH in numpy, the idle loop and lcd_fill shortcuts, --ms)
  (§80).

## To do

- More expressive messages beyond playback (a second step of 0.94's): the firmware installer
  (wrong CRC, another model, an image too large, low battery), the microSD card (a file system
  not read: "FAT32 or exFAT needed"), the USB session.
- Fuzzing targets for FLAC (dr_flac), ALAC and Musepack (§64); more varied seeds for the FAT
  target (its coverage stops growing after 20 minutes).
- The accelerators still to map (`docs/HIFIACC.md`, `docs/MP3ACC.md`): HIFIACC's ALAC filter
  (memory at `0x01069000`, stock module 79), its APE decoder (module 80) and its 64-bit MAC (TRM
  chapter 26); Imdct36 for MP3 (mapped, no end flag found, §70). Each checked on the device with
  a comparison bench, with an emulator model. Done: LPC, FFT, SYNTH.
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
- Resume a queue made by PLAY held (a tree, the whole card) after a power off: today only a
  queue of one folder resumes (the top level is shown). To save: the folder held, the shuffle
  seed and order position, the track (its folder and cluster, to check it is the same).
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
- To check on the device rather than add: the 4.4 mm balanced output (the plug is detected,
  nothing is specific to it; check by ear, and whether the stock firmware sets the DACs
  differently for it).
- Backlight: always at full brightness, no PWM setting yet.

## Ideas, not decided

- Further formats, a distant prospect, only if libraries call for them:
  - DSD (DSF, DFF): native DSD through the I2S and the CS43131, or conversion to PCM;
  - APE (Monkey's Audio, SDK now BSD): the higher compression levels may be too heavy for
    core B;
  - WavPack (BSD reference decoder).
  WMA, TTA and MP2 are not considered (rare, or no permissive decoder).
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
- AAC: assembly optimisations, not a priority (core B at 23 MHz, the lowest step, since 0.92):
  a Cortex-M3 header for the fixed-point operations (smull/smlal; the decoder's ARMv5TE variant
  uses DSP instructions the M3 lacks), then hand-written Thumb-2 for the hot functions left by
  the QEMU profile (trans4m_freq_2_time_fxp_2 14 %, inv_long_complex_rot 9 %, §54; the FFT, 25 %,
  is on HIFIACC); no reference assembly exists in AOSP for this decoder.
- MP3: the IMDCT on Imdct36 (§70), once its end flag is found; small expected gain (MP3 already
  at 21-23 MHz).
- AAC in ADTS (`.aac`), a distant prospect: pvaacdec already reads ADTS headers; an ADTS mode in
  codec_aac.c reading the file directly (frames found by their headers, as codec_mp3.c does),
  the `.aac` extension, ID3v2 tags, a length estimated from the bitrate, an approximate seek, no
  gapless (ADTS has no encoder delay), a fuzzing target. None in the library.
- Ogg seek, a distant prospect (§75): core A passes the granule of the page it found to core B,
  which starts on the target's page without decoding the page before for nothing (some 100 ms
  less in Vorbis); Opus keeps an 80 ms pre-roll (RFC 7845). Also: fill the DAC ring with silence
  at a seek, so that the short gap is silence and not a repeat of the old sound.
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
