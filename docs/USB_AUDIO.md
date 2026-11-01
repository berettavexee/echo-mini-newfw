# USB audio: the ECHO MINI as a USB sound card

This page sums up what this project learned while turning the SNOWSKY ECHO MINI (Rockchip
RKNanoD, two Cirrus Logic CS43131) into an asynchronous, bit-perfect USB sound card: what works,
what failed, the hardware limits, the pitfalls, and how computers and phones actually behave. It
is meant to save time for anyone writing USB audio for this player or for a similar DesignWare
USB controller. The study behind it, in French, is `ETUDE.md` §83-§88 and §90; the code is
`fw/src/uac.c` (the class), `fw/src/usb.c` (the controller) and `fw/src/audio.c` (the DAC ring).

Status (2026-11-01):
- **full speed**: USB Audio Class 1, 16 or 24 bits, 44.1 or 48 kHz;
- **high speed**: USB Audio Class 2, 16, 24 or 32 bits, 44.1, 48, 88.2, 96, 176.4 or 192 kHz;
- asynchronous, with a feedback endpoint; **bit-perfect with Linux** (no sample dropped or
  repeated over 13 minutes of music, the feedback settling at the clocks' difference, about
  +40 ppm);
- phones (a Pixel 6a, an iPhone) play cleanly but **ignore the feedback**: the firmware then
  drops a frame now and then (inaudible, not bit-perfect);
- not done: HID media keys, the choice between disk and sound card at the cable's arrival, a
  screen with VU meters. Windows and macOS were never tried.

## The hardware

**The USB controller** is a Synopsys DesignWare USB 2.0 OTG ("DWC2") at `0x60080000`, interrupt 5
of the NVIC.
- `hwcfg2` = `0x0820cc42`: OTG, **slave mode** (no internal DMA: every packet goes through the
  FIFO by CPU reads), UTMI+ PHY; `hwcfg3` = `0x01340480`: **308 words of FIFO** in all;
  `hwcfg4` = `0x09e04061`. It enumerates at high speed.
- **The RX FIFO is fixed at 147 words** (588 bytes). `hwcfg2` bit 19 is 0: no dynamic FIFO
  sizing, and whatever is written to `GRXFSIZ` reads back 147. Every OUT packet, with its status
  word, must fit in there: **a larger packet is dropped by the controller without any error**.
  This is the main limit of the whole design (below). Log the value read back: earlier notes of
  this project assumed 256 words for days. The TRM (chapter 14) lists 308 × 35-bit words of
  FIFO, no internal DMA, 3 device endpoints besides endpoint 0, one periodic IN endpoint of up to
  64 bytes.
- The isochronous IN (feedback) endpoint uses the **periodic TX FIFO 1** (`DIEPCTL` TxFNum = 1,
  `DPTXFSIZ1`); 16 words are plenty for a 4-byte packet.

**The audio path**: the packets go straight into the DAC ring of the main core (core A), which
I2S1 reads by DMA towards the two CS43131 (`docs/CS43131.md`). The second core (core B, the
decoders) is not used. The DACs run from their own crystal through the RKNanoD's ACODEC PLL:
**the output clock is not the USB clock**, which is why the card has to be asynchronous (or
drop and repeat samples).

## The two profiles

| | Full speed | High speed |
|---|---|---|
| Class | UAC1 (`bcdADC` 1.00) | UAC2 (interface association, clock source 0x10, programmable) |
| Formats | alt 1: 16 bits, alt 2: 24 bits (3 bytes) | alt 1: 16 bits, alt 2: 24 bits in 4 bytes, alt 3: 32 bits |
| Rates | 44.1, 48 kHz (endpoint's sampling frequency control) | 44.1 to 192 kHz (clock's frequency control, common to all formats) |
| Data endpoint | OUT 0x01, isochronous asynchronous (`bmAttributes` 0x05), one packet a 1 ms frame, 196 or 294 bytes | OUT 0x01, isochronous asynchronous, 392 bytes at most: **16 bits one packet every 4 microframes (500 µs, `bInterval` 3); 24 and 32 bits every 2 (250 µs, `bInterval` 2)** |
| Feedback endpoint | IN 0x81, 3 bytes, **10.14** samples per frame, every frame | IN 0x81 (`bmAttributes` 0x11), 4 bytes, **16.16 samples per microframe**, every 8 microframes (1 ms, `bInterval` 4) |
| Controls | feature unit: mute, volume (master) | feature unit: mute, volume (master); clock: frequency, validity |

Both profiles: input terminal "USB streaming", 2 channels (front left, front right), feature unit
(mute and volume, −64 to 0 dB by 0.5 dB, applied on the DACs' attenuation, added to the player's
own volume set with VOL+ / VOL−), output terminal "Headphones" (0x0302). Bus powered, 500 mA
declared. Test VID/PID of pid.codes.

**176.4 and 192 kHz, and the RX FIFO**: a packet and its status word must fit in the 147-word
RX FIFO, so 392 bytes at most here. At 192 kHz:

| Interval | 16 bits (4-byte frames) | 24 or 32 bits (8-byte frames) | 24 bits packed (6-byte frames) |
|---|---|---|---|
| 500 µs | 97 frames, 388 bytes: fits | 776 bytes: **dropped by the controller, silently** | 582 bytes: 146 words + status, no margin |
| 250 µs | — | 49 frames, 392 bytes: fits | — |
| 125 µs | — | 200 bytes, but one packet in two missed (below) | — |

Version 0.98 kept 500 µs everywhere and left 176.4 and 192 kHz out. Test image 99J moved the
24- and 32-bit alternates to 250 µs (the interval belongs to each alternate setting, the host
follows it) and offers all six rates in every format. With an interval of 2 microframes, the
packets always come on the same parity, which keeps the endpoint's re-arming as simple as at
500 µs. Measured with Linux: the 6 rates in 16 and 32 bits, then 2 minutes at 192 kHz in 32 bits:
**no frame dropped or repeated**, about 6,000 interrupts a second at 192 kHz, 65 µs at most
(657 µs once, at a change of format). Another firmware for this player (RECHO) announced 192 kHz
in 24 bits; this is very likely how.

## The clocks: asynchronous mode

The DACs keep their own clock; the host is told how many samples to send through the feedback
endpoint.

- **The value comes from the ring's fill**, by a proportional-integral controller: target fill
  10 ms, the fill averaged over about 64 packets, 8 units (10.14) per frame of error, an integral
  of 1/256 unit per frame of error per millisecond, damping about 0.5, settled in a few seconds
  (simulated first for ±40 and +300 ppm). The value is clamped to ±1 % of the nominal. With
  Linux, the integral settles at the clocks' difference (about +40 ppm on the PC used) and the
  fill stays on target to the frame.
- **What failed first**: measuring the DACs' rate in host frames (DMA position read right after
  start-of-frame, over 4,096 frames). The frame number read from `DSTS` skips values: the
  measure was 13 to 33 % off and irregular, the value stuck at its −1 % limit, the ring ran dry.
- **Formats**: 10.14 in 3 bytes at full speed; 16.16 in 4 bytes, samples per *microframe*, at
  high speed (USB 2.0 §5.12.4.2). Linux guesses the format by comparing the value with the
  nominal rate (`/proc/asound/cardN/stream0` shows "Feedback Format"), so it is lenient.
- **On a change of rate**, the feedback packet already queued is replaced at once: a host reads
  the first values to check the format and the rate (Linux kept the old rate otherwise).
- **The feedback endpoint must answer at every interval.** Linux ignores a feedback endpoint that
  does not (empty packets, a missed interval): a packet the host did not collect stays in the
  FIFO and is aimed again at the next interval (`GINTSTS` bit 20, incomplete isochronous IN, at
  full speed; the current microframe's parity at high speed).

**Fallbacks**, needed anyway:
- beyond 4 ms of error, one frame per packet is dropped (or repeated) until back within 0.5 ms;
- a ring that runs dry or overflows is set again around the target, with silence;
- silence is always written 20 ms ahead of the last packet: a late host plays silence, not old
  sound; after 50 ms without a packet the stream is considered stopped (paused);
- **a host that ignores the feedback** (both phones tried, below): when the controller stays at
  its 1 % limit for 10 s, the feedback goes back to the nominal and the drift is corrected one
  frame at a time, whenever the averaged fill is more than one frame off, at least 100 ms apart
  (about one frame every 0.4 s at 50 ppm), instead of bursts of about 150 frames. Measured on a
  phone (test image 99J, 48 kHz in 32 bits): the mode engaged after 10 s at the limit, the excess
  fill was worked off at up to 10 frames a second, then about 23 frames per 10 s, evenly.

## Real time

- **The USB is served from its interrupt** (`OTG_IRQHandler`): received packets go into the ring,
  endpoints are re-armed, feedback is sent there. The main loop only draws the screen, reads the
  keys, writes the volume and the log. What both share (silence written ahead, the format, the
  DMA position) is done with the interrupt masked at the NVIC (`usb_irq_block()`).
- Before that (UAC1, polled), every millisecond counted: drawing one line of text took 1.4 ms
  and packets were lost (each hole filled by repeated frames, heard as high beeps), and so did
  each I2C write to the DACs (about 1.1 ms) during volume ramps. Polling hooks inside the
  drawing and I2C code fixed it; the interrupt made them unnecessary.
- **High speed, one packet per microframe failed**: re-armed from the interrupt, every other
  packet was missed. One packet every 4 microframes (500 µs) works.
- **Parity**: an isochronous endpoint is armed for an even or odd (micro)frame. At high speed,
  the parity to aim at comes from the frame number in `GRXSTSP` of the last packet received; the
  incomplete-isochronous-OUT events of the intermediate microframes are ignored; if nothing
  arrives for 2 ms, the parity is flipped (the first packet aimed wrongly, counted as missed).
- Measured: about 1,870 interrupts a second at full speed (32 µs at most); at high speed about
  3,000 (500 µs packets) to 5,000 (250 µs) for the data and the feedback, 65 µs at most.
- `ticks()` read inside an interrupt can go back by about 1 ms while the SysTick interrupt is
  pending: discard negative durations.

## Pitfalls met

- **No sound**: the USB start-up copied pin settings from a mass-storage driver (GPIO2_0 at 1,
  GPIO1_4 at 0). GPIO2_0 at 1 silences the jacks and the DACs want GPIO1_4 at 1. Restore them after
  the USB start, or better leave them alone (`keep_audio_pins`): toggling them while the DACs
  run clicks.
- **A click when entering the mode**: those pins, and the volume going from silence to the
  setting at once. Volume changes always go through a ramp (3 dB every 2 ms); the format change
  cuts at once (the host has already stopped the old stream) and ramps back up.
- **24-bit packets never arrived** at 176.4 / 192 kHz with 500 µs packets, while the feedback
  flowed: the RX FIFO limit above. Nothing in the controller reports it. 250 µs packets fixed
  it.
- **False pause**: the loop tested `millis() - last_packet_ms > 50`; a packet arriving between
  the two reads put `last_packet_ms` ahead of the `millis()` value read, the unsigned difference
  became huge and the stream was declared stopped, then realigned (a possible small glitch).
  Read the shared time first, then `millis()`.
- **Test tones need a fade** in and out (50 ms): a sine that starts or stops at once clicks, and
  it is not the card's fault (`sox ... fade t 0.05 D 0.05`).
- **24-bit test files**: the card offers S16_LE and S32_LE to ALSA (24 bits in 4 bytes at high
  speed); `aplay -D hw:` refuses a packed 24-bit (S24_3LE) file.
- **One channel silent on an iPhone**: the iPhone's own Balance setting (Accessibility → Audio &
  Visual) was all to the right; iOS then sends exact zeros on the left. Check it before
  suspecting the device: the diagnostic build's per-channel peaks show it at once.

## Hosts

| Host | Profile | Result |
|---|---|---|
| Linux PC (ALSA, snd-usb-audio) | UAC2 high speed and UAC1 full speed | follows the feedback to the frame (a forced +0.5 % / −0.5 % step: 48,237.6 / 47,758.1 Hz); bit-perfect; recognises "ASYNC", the sync endpoint and its format |
| Pixel 6a (Android) | UAC2, 48 kHz | plays cleanly; reads the feedback endpoint every millisecond but **ignores its value**: always 24 frames per packet even with −1 % asked for minutes; its clock about 50 ppm off ours. Reported: https://issuetracker.google.com/issues/579970942 |
| iPhone (iOS) | UAC2, 44.1 kHz, 32 bits chosen | plays cleanly; **ignores the feedback value** in every format tried (+0.5 %, −0.5 %, +0.05 % in 16.16 per microframe; +0.5 % in 16.16 per ms and in 10.14 per ms): always 22.05 frames per packet on average; but **a 3-byte feedback packet makes it stop the stream** within 110 ms, so it does read and check them. First enumeration took about 20 s (a permission on the phone, probably), later ones 180 ms. It opens and closes the streaming interface very often |
| Windows, macOS | — | not tried |

Both phones request the clock, mute and master volume only; neither addresses the per-channel
controls, nothing is stalled.

## Diagnostics

**Test build** `make EXTRA=-DECHO_UACDIAG` (version "0.98ud"), on top of the normal log:
- every 10 s: the host's real rate (frames received, before any correction, timed on the
  player's crystal), the frames-per-packet histogram (`24:20003`: a host that follows the
  feedback mixes sizes), each channel's peak (`peaks L … R …`, of 32,767);
- every control request of the host (`uac: req …`) and the stalled ones;
- feedback steps after 20 s of stream, 20 s each: +0.5 %, −0.5 %, +0.05 % in 16.16 per
  microframe, then +0.5 % in 16.16 per ms and in 10.14 per ms; the figures are logged at each
  change. Do not use it for listening on a computer: the last two steps disturb a host that
  follows the feedback.

**The normal log** (`NEWFWLOG.TXT`, read with `fw/tools/readlog.py`) gives, every 10 s of stream:
packets, dropped / repeated / realigned frames, missed packets, the fill range, ring underruns,
the output's real rate measured on the DMA, the feedback value, the error and the integral
(stuck at ±1 % of the nominal: the host ignores the feedback), interrupt counts and the longest
part of the loop. Caveat: the log is not written to the flash while a stream plays and its buffer
can overflow; long sessions lose their beginning.

**From a Linux PC**, the card is "ECHO MINI":
- `aplay -q -D hw:N,0 file.wav` (S16_LE or S32_LE), `amixer -c N` for the host volume;
- `/proc/asound/cardN/stream0`: the formats, "Momentary freq" (the rate the host currently sends,
  from the feedback) and "Feedback Format";
- `sox -n -r 48000 -b 32 -e signed -c 2 t.wav synth 30 sine 440 sine 660 remix 1v0.5 2v0.25 fade
  t 0.05 30 0.05` makes a stereo test with different levels per channel.

## The other firmwares

Studied for ideas only (`ETUDE.md` §83):
- **stock firmware (V3.9.0)**: UAC1 full speed, 16 bits, **48 kHz only**, adaptive endpoint (no
  feedback), plus a HID interface (play/pause, next, previous, volume, mute...) so the player's
  keys drive the computer; "USB Connection Options" at the cable's arrival; a separate "UAC
  volume".
- **EchoPod** (GPL-3): UAC1 full speed, 16 and 24 bits, 44.1 and 48 kHz, adaptive, the drift
  absorbed by dropping or repeating a frame per packet beyond 4 ms of error (target 20 ms).
Neither had a feedback endpoint or went beyond 48 kHz.

## Open points

- HID media keys; the disk / sound card choice at the cable's arrival; a screen with the format,
  the feedback, an "untouched stream" mark and two VU meters.
- Windows (its UAC2 driver expects a 4-byte feedback packet at full speed too, according to the
  Zephyr project's documentation) and macOS.
- Whether the phones ignore the feedback because their USB audio goes through a separate DSP
  path (offload): `adb shell dumpsys media.audio_flinger` during playback would tell for
  Android.
- The log written during a stream.
