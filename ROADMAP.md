# Roadmap

State of the ECHO MINI new firmware, kept up to date with each version. The details of every
step (design, measurements, tests) are in `ETUDE.md` (in French), by section (§).

Last update: 2026-11-07, version 0.101 (interface languages, the resume after a pause fixed,
ALAC on the accelerator).

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
| Tags and ReplayGain found after a large cover (Ogg, FLAC, ID3v2: up to 16 MiB) and in an APEv2 tag holding a cover | 0.96 | §81 |
| Settings screen: screen timeout, brightness (backlight PWM), tags or file names, ReplayGain auto and preamp, start-up, sleep timer, power off when idle, swapped volume keys | 0.96 | §81 |
| Brightness in ten perceptual steps (8 to 100 %, about x1.3 a step) | 0.97 | §82 |
| USB sound card: UAC1 at full speed, 16 and 24 bits, 44.1 and 48 kHz, asynchronous with a rate feedback, bit-perfect (tested with Linux) | 0.97 | §84-§85 |
| USB sound card at high speed: UAC2, 16, 24 and 32 bits, 44.1 to 96 kHz, bit-perfect (tested with Linux); UAC1 kept at full speed | 0.98 | §87 |
| USB sound card: 176.4 and 192 kHz in every format (24/32-bit packets every 250 us); phones that ignore the feedback handled with evenly spread corrections; tested with a Pixel 6a and an iPhone | 0.99 | §90, §92, §93 |
| Pop-free power-down of the DACs (CS43131 datasheet sequence) at the long pause and the stop | 0.99 | §91 |
| Tags after a big cover: M4A over 512 KiB, Ogg comment headers up to 64 MiB (a 17.9 MB cover: tags in 1.4 s, read during the track before, no gap) | 0.100 | §96 |
| Plain error messages beyond playback: installer (incomplete, too large, damaged, battery), microSD card (not responding, needs FAT32 or exFAT), USB disk screen | 0.100 | §96 |
| A queue made by PLAY held (a tree, the whole card) resumes after a power off, same track, same shuffled order (18,257 tracks in 1,396 folders walked again in 2.4 s) | 0.100 | §96 |
| USB sound card as a headset remote (HID): PLAY play/pause, VOL+ or VOL- twice next or previous, MENU the remote on and off (kept); tested with VLC and YouTube on Linux | 0.100 | §95 |
| USB sound card: the Power key (screen off and on; held 2 s, the card left, then the power off) | 0.100 | §95 |
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
| Sample rates from 8 to 192 kHz played as they are (the stock firmware's 15 rates, three PLLs corrected: 176.4 kHz was 0.28 % slow until 0.98); upsampling for unusual rates; the real output rate in the log | 0.78, 0.79, 0.98 | §59-§60, §88 |
| Musepack SV7 and SV8, APEv2 tags, exact seek | 0.80 | §61 |
| Decoders hardened against damaged files; a fuzzing bench (Ogg, Vorbis, Opus, MP3, AAC, MP4, tags, FAT) | 0.81-0.83, 0.91 | §62-§64 |
| The chip's accelerators: FLAC LPC (HIFIACC), MP3 synthesis (SYNTH), Vorbis IMDCT (HIFIACC FFT), AAC IMDCT FFTs (HIFIACC), Musepack synthesis (SYNTH): 28 to 65 % less core B load | 0.85-0.93 | §66-§77 |
| ALAC's adaptive predictor on HIFIACC (orders 3 to 30; checked against our decoder on the device): core B at 9.5 MHz on 16-bit ALAC instead of 35; the end of Imdct36 and SYNTH as pending IRQs | 0.101 | §113-§116 |
| Interface languages (gettext PO files, catalogues in the image, the Language setting): English, French, Japanese and Simplified Chinese (partial) | 0.101 | §112 |
| Automatic checks (`make check`, pre-commit hook, GitHub Actions); images reproducible byte for byte; emulator tests in the CI on a shareable test set (CC0 recording) | 0.91, 0.92, 0.99 | §94 |

## The rework (0.99r1-r37, merged in 0.100)

A rework of how the work is shared between the two cores (core B: the whole PCM path to the
DMA; core A: one loop with a time budget, short interrupts), in seven steps, each one checked
in the emulator, then on the device, on the branch `rework`; merged into `main` with 0.100.

| Step | Versions | Checked | ETUDE |
|---|---|---|---|
| 1. Measure: the longest gaps of the audio path and of the loops, with the operation behind them, the rings' lowest fill (`fw/tools/perfreport.py`, the emulator's `perf-*` scenarios) | r1-r4 | device (77 min session, r4) | §97 |
| 2. One loop and a stack of screens: the screen wakes without blocking (the 120 ms gap gone), menu, settings and clock screens served by the main loop, the sleep timer's fade a step of the loop | r5-r9 | device (r9) | §99 |
| 3. A storage task (a coroutine, 4 ms a pass): the next track's tags read ahead (a 17.9 MB Ogg cover no longer between two tracks), the tree walk (the whole card, 18,257 tracks, 27 ms at most out of the audio path), saves after the frame; a seek keeps the tags | r10-r16 | device (r15, r16) | §100-§101 |
| 4. Core B writes the PCM ring the DMA reads (96 KiB in HIGHRAM1; core A no longer copies PCM), the input ring 96 KiB in SYSRAM (96 KiB left free for the screens), the battery beeps mixed by core B; r23: the resume of a tree queue plays its track at once, the tree walked meanwhile | r17-r23 | device (r21) | §102-§104 |
| 5. The input ring in segments (a jump core B cannot serve is a request core A answers: back, forward, Musepack's seek table), a chained track and a seek opened while the loop goes on, gapless checked sample for sample; r26: the card mounted at power-on | r24-r26 | emulator; the card at power-on and album chains on the device (r30, r31) | §105-§106 |
| 6. Core B's clock: our feedback loop kept, with finer rules (a window of a quarter of the PCM ring; the ring under half a level up, under a quarter the top; over 85 % load up at once, under 65 % for 1 s straight down to the level for 75 %); core B lighter (its SysTick only while playing); r30-r31: core B's load in 24-bit WAV halved again (192 kHz: 63-66 MHz since r18, now 33 MHz), by a shorter PCM ring write and SYSRAM read by aligned words | r27-r31 | device (r30, r31) | §107-§108 |
| 7. Robustness: a fault record in a sector of NEWFWLOG.TXT written by the fault handler (it survives a power off; a "Fault" screen once at the next boot); core B reloaded after a fault or 500 ms stuck, the same track again 1 s before (passed over the second time); the watchdog at 2.7 s, its interrupt writing the record; an `ECHO_FAULT` test image | r32-r36 | device (r34f-r36f) | §109 |

Margins measured (§103, §106): a 500 ms stall of core A causes no underrun in MP3, Vorbis or
AAC (3 to 5 s of protection); the tightest case, WAV 24/192, stands about 150 ms. Long tracks:
1 to 4 clock changes of core B instead of 3 to 19, no underrun.

Checked on the device with r31: the resume of a tree queue over three power cycles (the whole
card, 18,257 tracks: the track at once, the tree walked in 3.4 s meanwhile), seeks, the sound
card (16/44.1, 24/96, 24/192, no packet lost). Musepack SV7 (no seek table): a seek reads the
file from its start again on core B (360 to 730 ms of silence, the loop free); kept as it is,
SV7 being rare (decided 2026-11-06).

Left from the rework:
- a long session with the normal image (no fault record, no watchdog reset expected);
- the keys read in the SysTick interrupt;
- the screens that stop the music (USB, install, sound card, system) still in loops of their
  own;
- still blocking the loop: a folder opened by the browser, a card put in during playback
  (mounted at its first use, up to 253 ms);
- at 176.4 and 192 kHz the clock rules swing between 50 and 80 MHz (10 changes a track, the
  ring kept over half); a DMA copy from SYSRAM would save about 5 MHz more there (§108);
- the next track prepared in the input ring, without restarting core B: left out (nothing
  audible to gain today) unless a need shows.

Ideas from the other projects for it (ETUDE §98): still screens not redrawn and the loop in WFI
(EchoPod), cold code built for size (Podium), a wired headset remote on the 3.5 mm mic contact
(Podium; a feature, not part of the rework).

## Waiting for a test on the device

- A steady battery percent (0.97): the mean of a minute of readings, moving freely only down on
  battery and up on USB, never 100 % while charging (§82); to check: no jump when a track
  starts or the screen comes on, nothing odd when plugging and unplugging the cable. A first
  short session (full battery) showed no regression; a long one, half charged, is still needed.

- From pull requests #9 and #1 (CHEYAN): a track saved paused at position 0 now comes back
  paused (it was lost; emulator: `emutest.py resume-paused-zero`), and a track resumed at
  power on opens the play screen instead of the browser. On the device: "At power on: resume
  paused", a track paused in its first two seconds, then the player switched off and on twice.

- From pull request #8 (CHEYAN): the browser's selected row scrolls when its name is too long,
  a file or a folder, with the play screen's timings. On the device: a long album folder and a
  long file name, the scroll reset when the selection or the page changes.

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
- 0.96: the settings screen (screen timeout, brightness by PWM, tags or file names, ReplayGain
  auto and preamp, start-up, sleep timer, power off when idle, swapped volume keys); shuffle
  starts from the track chosen; fixes: tags after a cover of 512 KiB or more (Opus whose
  comments put the picture in the middle) and Musepack tags with a cover were lost (§81). New
  emulator scenarios: a real album's ReplayGain, a library of 18,260 tracks in 1,396 folders,
  large covers; two scenarios at a time at most.
- 0.97: brightness in perceptual steps, steady battery percent (§82); the USB sound card: UAC1
  at full speed, asynchronous, then on the USB interrupt (§83-§86).
- 0.98: the USB sound card in UAC2 at high speed, 44.1 to 96 kHz in 16, 24 and 32 bits (the RX
  FIFO stays at 147 words: no larger packets) (§87); 176.4 kHz played 0.28 % slow since 0.16
  (the stock firmware's PLL beyond its VCO's range), now exact; the output rate measured in the
  log, the 15 rates checked (§88).
- 0.99: the main PLL's VCO moved from 400 MHz, the bottom of its range, to 1.2 GHz (still
  400 MHz out), the core clock measured against the crystal (§89); the USB sound card on phones:
  a Pixel 6a and an iPhone ignore the feedback in every format, now detected and corrected one
  frame at a time (§90, §92); a false pause fixed; 176.4 and 192 kHz in every format, 24/32-bit
  packets every 250 us (§93); pop-free DAC power-down (§91); docs/USB_AUDIO.md, docs/CS43131.md.
- 0.100: the rework (§97-§109): one main loop and a stack of screens (the screen wakes without
  a 120 ms block), a storage task (the next track's tags read ahead, the tree walked while the
  music plays), core B writes the PCM ring the DMA reads (96 KiB of SYSRAM freed), the input
  ring in segments (a chained track and a seek open while the loop goes on, gapless checked
  sample for sample), the card mounted at power-on, finer clock rules for core B and its load
  in 24-bit WAV halved, a fault record, core B restarted after a fault or a hang, a watchdog;
  from `main` since 0.99: the sound card's remote and Power key, plain error messages, the
  resume of a tree queue, tags after big covers, pull requests #2 #7 #8 #9 #10 (§95, §96); the
  installer with long names (§110).
- 0.101: interface languages (gettext PO files, catalogues in the image, the Language setting:
  French, Japanese and Simplified Chinese, from pull request #1) (§112); the resume after a
  pause fixed (core B was not woken since 0.99r28, then read the pause as sound played)
  (§115); ALAC's predictor on HIFIACC, core B 3.7 times lighter on ALAC (§113-§116); the
  accelerator benches and their emulator models (§113-§114); docs/BATTERY.md (§115).

## To do

- Screen writes by DMA: a band (for instance 320 x 16) drawn into a buffer, then sent to the
  panel by the DMA, instead of the CPU writing each pixel: a scrolling line (the long title's
  32-pixel marquee every 33 ms, since PR #2) and full redraws (11 to 14 ms today) would no
  longer hold core A. After the rework (its one loop, step 2, makes room for banded drawing).

- Fuzzing targets for FLAC (dr_flac), ALAC and Musepack (§64); more varied seeds for the FAT
  target (its coverage stops growing after 20 minutes).
- The accelerators (`docs/HIFIACC.md`, `docs/MP3ACC.md`): mapped from the stock modules but not
  yet checked on the device (§113): HIFIACC's APE filters (module 80), the FIFOs without DMA,
  Imdct36's mode 1 (saturates). Still unknown: the 64-bit MAC (no stock module uses it), the
  interrupt line of HIFIACC or DMAC2 (core B could sleep through long LPC blocks, §116). Done:
  LPC, FFT, SYNTH, the ALAC filter, Imdct36's end (IRQ 7, §116).
- Actions on the plug events (to be proposed and agreed first):
  - pause when the headphones are unplugged;
  - a USB screen to mount the internal memory or the card, read only or read/write.
- USB access to the microSD card (step S5 of §36).
- USB sound card (done so far: the table above; history in `docs/USB_AUDIO.md`, §83-§93):
  - entered from the USB screen when the cable comes in (the main menu only today);
  - HID media keys (done, §95): to check with Windows, macOS, Android and iOS hosts; the
    MENU redraw of the remote's line holds the loop 13 ms (harmless, the packets come in by
    interrupt; a smaller redraw would do);
  - a screen with the format received, the rate fed back, an "untouched stream" mark and two
    VU meters, one per channel (a silent channel shows at a glance); power saving off in this
    mode (USB-powered);
  - phones: the Pixel 6a and the iPhone ignore the feedback (handled since 0.99, §92); on the
    Pixel, check the offload (`adb shell dumpsys media.audio_flinger`) and try a commercial
    asynchronous DAC; reported to Google: https://issuetracker.google.com/issues/579970942;
  - power: 500 mA declared with a computer (charging while playing), 100 mA with a phone,
    which may refuse more (iPhone); how to tell them apart is still open;
  - Windows: no host at hand, to be reported by users;
  - later: DSD over PCM (DoP), if the link to the CS43131 allows it; a composite device, sound
    card and mass storage at once.
- A diagnostics, telemetry and remote-control module over the USB link with the PC, to automate
  the tests on the device: read the log and the state live (playback, buffers, core B load,
  battery, keys), send key events and commands (play a file, seek, change a setting), and run
  scripted test sequences from the PC (a vendor USB interface or a second function beside the
  mass storage, to be designed).
- The charging lift: the voltage read while charging, against the same charge without the
  cable, over a logged charge, for a correction of the percent shown while charging (EchoPod
  takes 50 mV off; 0.97 only stops at 99 %).
- Key lock: a special key combination locks and unlocks the keys, against accidental presses
  in a pocket or a bag (the combination, what stays active when locked and what the screen
  shows to be proposed and agreed first; the key events of keys.c make it a filter in one
  place).
- The emulator (`fw/tools/emu_fw.py`, `fw/tools/emutest.py`, `work/emu/scenarios.py`). Done
  (§94, §96): installer tests with power cuts, self-checking scenarios and a CI step (`boot`,
  `play-wav`, `play-flac`, `install-badcrc`, `set-ci`, Musepack SV8 in the test set), a clock
  from the instructions run (`--cycles`) with TIMER1 on both cores, eMMC and microSD latency
  (`--mmc-lat`, `--sd-lat`), the eMMC's fitted on the device and the default with `--cycles`
  (an install within 7 %; the default mode's clock suits behaviour, not durations), the
  HIFIACC DMA chain; with the rework: core B's SysTick and the DMA position it reads, core B
  run at its clock (`--b-cpi`, its load measurable), the PCM ring's fill traced
  (`--ring-trace`), a card mounted at power-on (`sd-boot`) (§103-§107). To do: fit `--sd-lat`
  (the device walks 18,257 tracks on the card in 2.37 s, mounts it in 253 ms), which needs a
  card image of that size (1 below). Ideas from the other projects' emulators (Podium,
  EchoPod, ReChord; ideas only, their code is GPL-3), by usefulness:
  1. volumes generated on demand (a sparse disk whose files, valid FLAC frames, are made when
     read): libraries larger than the PC's RAM, cluster sizes (512 bytes), the files a Mac leaves
     (Spotlight, fseventsd, `._` files);
  2. sector reads counted by phase (FAT or data), to catch regressions;
  3. a finer panel model: drawing windows and sweeps followed, full redraws and flicker counted,
     the time of the first frame;
  4. a check that the ELF matches the image (wrong symbols otherwise);
  5. a simulated USB host (enumeration, isochronous packets, feedback, the HID reports), to
     test the sound card without the device (today `uac-power` only enters its screen);
  6. later: an interactive mode (a live window, the PC's keys), QEMU for the whole image (speed).
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
  - key layouts: modes, volume on short or double press (swapped volume keys: 0.96).
- Settings and system:
  - settings: a text file on the internal memory to set them from a computer (they are kept in
    the resume state's sector today, §81); a default play mode;
  - an information screen: version, supported formats, free space on the internal memory and
    the card;
  - delete a file from the browser (needs writing to FAT32 and exFAT: read only today);
  - format the internal memory or the card, reset the settings;
  - interface languages besides English (the font already covers Latin, Greek, Cyrillic and
    CJK): gettext PO files, open to Weblate (docs/TRANSLATING.md, ETUDE §112). Done in the
    emulator and on the device (0.101): the strings marked, the catalogues in the image, the
    Language setting, French (complete), Japanese and Simplified Chinese (81 of 153 strings from
    pull request #1, the rest proposed, fuzzy); to do: a review of the Japanese and Chinese, the
    Weblate project;
  - colour themes (minor);
  - daylight saving time: the change between summer and winter time (the real-time clock keeps
    local time today, set by hand on its screen, §28).
- To check on the device rather than add: the 4.4 mm balanced output (the plug is detected,
  nothing is specific to it; check by ear, and whether the stock firmware sets the DACs
  differently for it).

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
    in core B's data today; more with the PMU-SRAM and the SYSRAM, which core B reads and
    writes, checked on the device, §102);
  - enough for most MODs, the small S3Ms, few ITs.
- Album covers (skipped today).
- M3U playlists.
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
  less in Vorbis); Opus keeps an 80 ms pre-roll (RFC 7845). (The ring filled with silence at a
  seek: done in the rework, §103.)
- Bluetooth output, a prospect: the 8 GB model has a Bluetooth module with its own stack,
  powered from GPIO2.7 and driven over UART1 at 115,200 baud with short ASCII commands; GPIO2.0
  and GPIO2.14 route the sound (EchoPod streams to headphones this way, and found that the
  routing pins fake jack plug events). Needs a UART driver, the module's command set, pairing
  screens and the routing of the DAC output.
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
- Tags placed after a large cover take longer to read: about 130 ms for an Opus with a 960 KB
  picture before its title (Ogg pages of 4 KiB must all be crossed); tags beyond 16 MiB are not
  read.
- Ogg Vorbis with floor 0 (libvorbis betas of 2000) or with blocks over 4096 samples (no current
  encoder makes them; Tremor lowmem decodes them wrongly) is refused (§62).

## Not planned

- Equaliser.
- The extras of the stock firmware's generic Rockchip SDK base (e-book reader, photos, video,
  dictionary, calculator, FM radio, recording): mostly absent from this hardware (no radio or
  microphone as far as known). Bluetooth exists, see the ideas above.
- Any dependency on the stock firmware image.
