# Battery life

How long the ECHO MINI plays on one charge with the new firmware, version after version. Each
run is a long listening session on battery, read from the firmware's log (`NEWFWLOG.TXT`): the
firmware logs the battery voltage once a minute, and powers off when the battery stays under
3.30 V for 30 seconds.

![Battery voltage against time on battery, one line a version](battery/battery-life.svg)

<!-- runs -->
| Version | Date | Start | Hours on battery | Hours to 3.6 V | End | Played | Screen on | Cable |
|---|---|---|---|---|---|---|---|---|
| 0.72 | 2026-10-25 | 4.18 V | 8.2 | - | none (log ends) | 8.2 h: Opus 54 %, Vorbis 38 %, MP3 6 %, WAV 2 %, FLAC 0 % | 22 min | 0 min |
| 0.80 | 2026-10-26 | 4.18 V | 15.6 | 10.0 | battery empty | 15.5 h: Vorbis 44 %, MP3 42 %, FLAC 13 %, MPC 1 % | 9 min | 11 min |
| 0.87fx1 | 2026-10-28 | 4.18 V | 7.3 | - | none (log ends) | 7.7 h: Vorbis 45 %, FLAC 27 %, MP3 20 %, MPC 9 % | 17 min | 0 min |
| 0.95 | 2026-10-29 | 4.18 V | 10.1 | 8.9 | no key for 10 min | 10.1 h: Vorbis 47 %, FLAC 28 %, MP3 16 %, MPC 9 % | 100 min | 0 min |
| 0.99r37 | 2026-11-06 | 4.19 V | 14.6 | 9.3 | battery empty | 14.6 h: MP3 72 %, Opus 23 %, Vorbis 5 %, MPC 1 % | 5 min | 0 min |
| 0.104k1 | 2026-11-12 | 4.19 V | 14.5 | 9.4 | battery empty | 14.5 h: MP3 54 %, Opus 23 %, Vorbis 23 % | 0 min | 0 min |
<!-- /runs -->

- **Hours on battery**: from the first battery line without the USB cable to the last one.
- **Hours to 3.6 V**: comparable between runs that did not run the battery flat.
- **End**: `battery empty` is a full run (charged, then played until the power off); the other
  runs were stopped earlier (`no key for 10 min`: the playlist ended), or the log does not hold
  their end.
- **Played**: the time of music played in the session, by format. Formats do not cost the same:
  Opus and Vorbis keep core B busier than MP3 (whose synthesis runs on the chip's accelerator).
  Two runs are only comparable with a similar mix.
- **Screen on**: the time the screen was lit. The backlight costs more than decoding.
- **Cable**: the time the USB cable was plugged in during the run (to copy the log), left out of
  the hours; the battery charged meanwhile, so such a run lasts a little longer than it should.

The runs are not controlled measurements: the music, the volume, the headphones and the age of
the battery change from one run to the next. Read the table as a trend, not as a benchmark.

## What the runs show

- **0.99r37 and 0.104k1**, the two full runs with the screen off nearly all the time, last the
  same: 14.6 h and 14.5 h, 9.3 h and 9.4 h to 3.6 V. Over the first 14 hours their curves stay
  within 15 mV of each other (5 mV on average; 3.85 V after 4 h, 3.63 V after 8 h, 3.53 V after
  12 h, 3.40 V after 14.2 h), then part by up to 34 mV in the last minutes, where the voltage
  falls fast. Between them came the screen drawn by DMA, the code overlays and the keys read
  in the SysTick interrupt (0.104k1): none of them shows in the battery life. 0.104k1 played
  more Vorbis (23 % against 5 %, the heaviest format for core B), which does not show either.
- **0.80** ran an hour longer (15.6 h), with 11 minutes on the cable that charged it meanwhile.
- **0.95**, with 100 minutes of screen on, reached 3.6 V after 8.9 h instead of 9.3 to 10: the
  backlight costs more than the decoding.

## Adding a run

1. Charge the player fully, unplug it, play music until it powers off (or for at least six
   hours).
2. Copy `NEWFWLOG.TXT` from the player before it fills again (the log is circular: about one
   day of playback).
3. Extract the session and draw the chart again:

   ```sh
   python3 fw/tools/batlife.py extract docs/battery NEWFWLOG.TXT
   python3 fw/tools/batlife.py plot docs/battery
   ```

   `extract` writes one CSV per session of six hours or more (`docs/battery/<date>_<version>.csv`:
   the seconds on battery and the voltage, and in comments the version, how the session ended,
   the formats played and the screen's time on). `plot` draws `docs/battery/battery-life.svg`
   and rewrites the table above.
