# SYNTH and Imdct36: the RKNanoD MP3 accelerators

Two small accelerators of the RKNanoD, separate from HIFIACC (see `HIFIACC.md`), that the stock
MP3 decoder uses. This page gives what this project established about them. The study, in
French, is `ETUDE.md` §68 to §71.

Status: **SYNTH is understood, verified on the device and used** by this firmware since 0.87
(the MP3 synthesis filter bank, about 42 % less load on the HiFi core). **Imdct36 is mapped but
not used.**

## Where they are

TRM chapters 27 (SYNTH) and 28 (Imdct36) describe their memories; the registers are not
documented. Addresses seen by core B, the HiFi Cortex-M3 (the TRM gives the core A addresses
`0x62044000` and `0x62042000`):

| Block | Base | Clock gate | Reset |
|---|---|---|---|
| Imdct36 | `0x60010000` | `hclk_imdct` (gate 7·16+14) | `SOFTRST0` bit 14 |
| SYNTH | `0x60050000` | `hclk_synth` (gate 6·16+14) | `SOFTRST0` bit 15 |

The register use below comes from the stock MP3 module (overlay 73 of firmware V3.9.0), then was
checked on the device.

## SYNTH: the subband synthesis filter bank

It does the whole MP3 polyphase synthesis (matrixing, window, history) and returns final 16-bit
PCM.

| Address | Use |
|---|---|
| `+0x000` to `+0x07c` | SRAM0, 32 words: the 32 subband samples of a slot in; the 32 PCM samples out (low 16 bits) |
| `+0x800` | 1: on (0: off) |
| `+0x804` | `channel \| ((15 - phase) << 1)`, before each slot |
| `+0x80c` | becomes nonzero when the slot is done; write `0x11` to acknowledge |
| `+0x810` | write 0 after each slot |
| `+0x1000` to `+0x1ffc` | SRAM1, the history (1,024 words); clear it at the start of a stream and after a reset |

Sequence for one slot of one channel (18 slots per granule):

1. `+0x804` = channel (bit 0) and `15 - phase` (bits 1-4); each channel has its own phase,
   starting at 0 and moving on by 1 (modulo 16) per slot.
2. Write the 32 subband samples to SRAM0: the computation starts by itself.
3. Wait for `+0x80c` to become nonzero (a few microseconds; the stock module uses the interrupt
   instead).
4. `+0x80c` = `0x11`, `+0x810` = 0.
5. Read the 32 PCM samples from SRAM0 (low 16 bits of each word).

Input scale: SYNTH takes its input 16 times larger than PacketVideo's MP3 decoder (pvmp3dec,
AOSP) keeps its subband samples, so this firmware writes them `<< 4`. The stock module, based on
libmad, writes its Q28 samples as they are.

Accuracy and speed on the device (`ETUDE.md` §70): over 1.3 million slots, no timeout; 99.995 %
of samples within 1 LSB of pvmp3dec's software synthesis, 2 LSB at most; the MP3 load on the HiFi
core goes from 37-39 MHz to 21-23 MHz (44.1 kHz, *Discovery*), SYNTH busy about 7 % of the time.

## Imdct36: the 36-point IMDCT

| Address | Use |
|---|---|
| `+0x000` to `+0x044` | 18 inputs: the computation starts on writing them |
| `+0x048` to `+0x0d4` | 36 outputs (read only) |
| `+0x800` | 3 at start in the stock module, 0 to stop |
| `+0x80c` | written 0 by the stock module's interrupt handler (acknowledge) |

- It computes the IMDCT of 36 points only: the window, the overlap-add and the frequency
  inversion stay in software (the stock module does them).
- `+0x800` = 1 returns the transform at the input's scale; `+0x800` = 3 divides it by about
  4,096 (the TRM's "pre-right-shift" mode, which suits libmad's Q28 format).
- Accuracy: 20-bit coefficients; only about 27 % of the outputs within 1 LSB of an exact
  transform.
- **No end flag found**: `+0x80c` never changes when polled; the stock module relies on the
  interrupt. Results were right after 200 µs.

Not used by this firmware: pvmp3dec's 18-point MDCT with window and overlap takes 16 µs per
subband, and its history format would force a rewrite of the whole IMDCT stage for a small
expected gain. Finding the end flag (or using the interrupt) is the open question.

## Test bench

`make EXTRA=-DECHO_HWTEST` builds an image that computes every synthesis slot on SYNTH and
compares it with pvmp3dec, and compares one transform in 16 on Imdct36 with an exact IMDCT
(`fw/hifi/hwtest_synth.c`); `make EXTRA=-DECHO_HWSYN` plays with SYNTH and compares the first
100 granules of each track. Results go to the device log (`hws:`, `hwi:`, `hwp:` lines).
