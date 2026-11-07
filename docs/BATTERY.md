# Battery life

How long the ECHO MINI plays on one charge with the new firmware, version after version. Each
run is a long listening session on battery, read from the firmware's log (`NEWFWLOG.TXT`): the
firmware logs the battery voltage once a minute, and powers off when the battery stays under
3.30 V for 30 seconds.

![Battery voltage against time on battery, one line a version](battery/battery-life.svg)

<!-- runs -->
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
