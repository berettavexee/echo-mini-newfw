# HIFIACC: the RKNanoD audio accelerator

HIFIACC is a fixed-point accelerator inside the Rockchip RKNanoD, the SoC of the SNOWSKY ECHO
MINI. This page collects what this project established about it, so that other developers can
use it without repeating the work. The study behind it, in French, is `ETUDE.md` §1.5 (register
map), §66 (on-device comparison) and §67 (use in the FLAC decoder).

Status: the **LPC filter used by FLAC** is understood, verified bit-exact on the device and used
by this firmware since version 0.85. The **FFT** is mapped, measured on the device and used since
version 0.88 for the IMDCT of the Vorbis decoder, and since 0.92 for the FFTs inside the IMDCT
of the AAC decoder. The **ALAC filter**, mapped from the stock firmware's ALAC module, is
verified on the device for orders 3 to 30 and used by our ALAC decoder since version 0.101. The
**APE filters** are mapped from the stock APE module (below; not yet verified on the device).
The MAC is not mapped: no stock module uses it.

## What it is

- A **Rockchip design**: the TRM (RKNanoD Technical Reference Manual, chapter 26) calls it "a
  design of fixed point accelerator". No other public source, driver or SoC mentions it.
- In the **PD_HIGH** power domain, with the second Cortex-M3 (core B, the "HiFi" core) and the
  HiFi RAMs. Its AHB clock is gate `hclk_hifiacc` (CRU gate G7_15); its reset is bit 3 of
  `CRU_SOFTRST(2)` (write `0x00080008` to hold it in reset, `0x00080000` to release it).
- Its DMA requests go to **DMAC2** (a DesignWare AHB DMA controller at `0x01070000`, two
  channels): request 0 is "hifi rx", request 1 is "hifi tx" (TRM table 2-2).
- Functions listed by the TRM:
  - FFT/IFFT, 64 to 1,024 points;
  - MAC, 32 × 32 bits with a 64-bit result, 1 to 1,024 terms;
  - FLAC (fixed coefficients) and ALAC (variable coefficients) FIR, order "1 to 32";
  - an APE (Monkey's Audio) decoder, versions 3970 and 3990;
  - FIFOs of 32 words of 32 bits, reachable by DMA or directly by the CPU;
  - 192 kHz / 24 bits claimed at 300 MHz.

## The LPC filter (FLAC)

It computes the FLAC fixed-coefficient prediction:

```
y[n] = x[n]                                       for n < order   (warm-up samples)
y[n] = x[n] + (sum_{k=0}^{order-1} COEF[k] * y[n-order+k]) >> shift   otherwise
```

where `x` is the warm-up samples followed by the residuals, and `y` the decoded samples. The
products and the sum are 64-bit: 24-bit audio with 15-bit coefficients at order 32 is exact.

### Registers (not in the TRM)

Reconstructed from the stock firmware's FLAC module and from another custom firmware's driver,
then verified on the device.

| Address | Use |
|---|---|
| `0x01060000` | general configuration: `0x01080108`, written once |
| `0x01060004` | bit 15: LPC mode; set before a block, cleared after |
| `0x01060008` | status: bit 15 = block done |
| `0x0106000c` | `0xa0000000 \| (n << 15)`, then the same `\| 3` to start; `n` = block length, **13 bits (at most 8,191)**; write 0 after the block |
| `0x0106002c` | `(order << 16) \| (shift << 8)` |
| `0x01060040` | input FIFO: warm-up samples then residuals (32-bit words) |
| `0x010600a0` | output FIFO: decoded samples |
| `0x0106b000` | coefficient memory, 32-bit words, **in reverse order**: `COEF[k] = qlp[order-1-k]` (the TRM's `H(i) = h(N-1-i)`) |
| `0x01069000` | the ALAC filter's coefficients (below) |

### DMAC2 set-up

| | Channel 1: input | Channel 0: output |
|---|---|---|
| SAR | the input buffer | `0x010600a0` |
| DAR | `0x01060040` | the output buffer |
| CTL (low), last or single block | `0x02109125` | `0x00a09425` |
| CTL (low), chained block | `0x1a109125` | `0x18a09425` |
| CTL (high) | block length in words | block length in words |
| CFG / CFG_H | `0x800` / `0x807` | `0x400` / `0x007` |

Channel registers are at `0x01070000 + ch × 0x58` (SAR `+0x00`, DAR `+0x08`, LLP `+0x10`,
CTL `+0x18`/`+0x1c`, CFG `+0x40`/`+0x44`). Global registers: RawTfr `+0x2c0`, ClearTfr `+0x338`,
ClearBlock `+0x340`, ClearErr `+0x358`, DmaCfgReg `+0x398` (write 1 once), ChEnReg `+0x3a0`.

### Sequence for one block

1. Once: DmaCfgReg = 1; `0x01060000` = `0x01080108`.
2. Clear the DMA status (ClearTfr, ClearBlock, ClearErr = 3).
3. Write the coefficients (reverse order) and set the unused taps to 0.
4. `+0x04` |= `0x8000`; `+0x2c` = order and shift; `+0x0c` = `0xa0000000 | n << 15`.
5. Program both channels (see the DMA rules below).
6. `+0x0c` = `0xa0000000 | n << 15 | 3` (start); ChEnReg = `0x202`, then `0x101`.
7. Wait for RawTfr bit 0 (channel 0 done) and bit 15 of `+0x08`.
8. `+0x04` &= ~`0x8000`; `+0x0c` = 0.

The output may be written over the input (in place): each output sample follows its input sample
through the FIFO.

HIFIACC runs at core B's clock, at about one product per cycle: a block of 8,191 samples at order
32 takes some 5 ms at 50 MHz, 1.3 ms at 200 MHz. Allow for that in any timeout (this firmware
waits up to 30 ms; 5 ms timed out at low clocks).

### Pitfalls found on the device

- **Orders 1 and 2 are wrong.** The output runs away (by about 3 × the coefficient per sample),
  as if the filter always had at least three taps. Zeroing the unused taps does not help. The
  stock FLAC module never uses HIFIACC for orders 1 and 2 either: compute them in software.
- **A single DMA block must be in the channel registers.** Without a chain (CTL without the LLP
  enable bits) the controller does not read the linked-list item: SAR, DAR and the block length
  must be written to the channel itself. Otherwise the channel reuses the previous addresses and
  writes anywhere (our first test image froze core B until a reboot).
- **A chain starts from its first item.** With the LLP enable bits set, the channel loads its
  first item from LLP at start: LLP must point to item 0, not item 1.
- **The DMA block length is limited** (we use blocks of 2,048 words, chained).
- **Blocks over 8,191 samples** (the length field is 13 bits) run in passes: each later pass gets
  the last `order` outputs of the one before as its warm-up. The result is exact.

### Verified domain

On the device (`ETUDE.md` §66, about 4,000 subframes from test files and real albums): bit-exact
against a software decoder for **orders 3 to 32**, 16 and 24 bits, mono and stereo, coefficient
precision 5 to 15 bits, blocks of 1,152 to 8,192 samples (in passes above 8,191), no timeout.

Speed, against dr_flac's own prediction on the same subframes, DMA included: about **6 times
faster** in 16 bits at order 8, **15 to 16 times** at high orders and in 24 bits (where software
needs 64-bit arithmetic).

## The FFT

Measured on the device with the USB probe (`ETUDE.md` §72-73; uses: §74 Vorbis, §76 AAC).

| Address | Use |
|---|---|
| `0x0106000c` | bits 31-29: the function (0 = APE channel X, 1 = APE channel Y, 2 = FFT, 3 = ALAC, 4 = MAC, 5 = FLAC); `0x40000000`, then `0x40000003` to start, then **0 before reading the result** |
| `0x01060024` | `size << 29` (64: 0, 128: 1, 256: 2, 512: 3, 1,024: 4); bit 27: no scaling (0: the result is divided by N); bit 20: real input (the imaginary part is ignored) |
| `0x01061000` | imaginary parts, 1,024 32-bit words; the result in place |
| `0x01062000` | real parts, 1,024 32-bit words; the result in place |

- **Inputs in bit-reversed order, output in natural order**; the result is the forward DFT
  `X[k] = sum x[n] e^(-2 i pi k n / N)`.
- There is no inverse mode: compute `ifft(x) = conj(FFT(conj(x))) / N`.
- While the function field still says FFT, the data memory belongs to the engine: every read
  returns the same value. Write 0 to `+0x0c` first.
- Precision, random inputs of 15 to 24 bits, 64 to 1,024 points: largest error 0.003 to 0.008 %
  of the peak (twiddles of about 14 bits); no overflow seen up to 24-bit inputs at 1,024 points.
- The computation is done within a millisecond (the probe's round trip); the exact time is not
  measured yet.
- Verified only up to 24-bit inputs. Without scaling the result must fit 32 bits: the AAC use
  below shifts its input by the sum of the |re| + |im| (an upper bound of every output).

### Use: the IMDCT of Vorbis (0.88)

`fw/hifi/hififft.c` computes Tremor's `mdct_backward` for blocks of 256 and 2,048 with a FFT of
n/4 points (`ETUDE.md` §74): a pre-rotation `w * (X[2m] + i X[n/2-1-2m])`, `w[m] =
exp(-2 i pi (m + 1/8) / n)`, written in bit-reversed order, the FFT without scaling, then the
same rotation on the output, unfolded into Tremor's order. Other sizes stay in software, and so
does a block that times out (3 ms); after 3 timeouts, the rest of the session.

On the device: about 4 times faster than Tremor's own IMDCT; whole Vorbis tracks at 34-35 MHz
of core B instead of about 50 MHz (some -34 %); within 1 LSB of Tremor on 94 % of the 16-bit
samples, 9 LSB at most (about -71 dBFS). Overnight: 1.9 million blocks, no timeout.

### Use: the IMDCT of AAC (0.92)

`fw/hifi/aacfft.c` replaces the two FFTs of pvaacdec's IMDCT (`imdct_fxp.cpp`): 512 complex
points for long blocks, 64 for short ones; pvaacdec's own pre- and post-rotations stay. Its FFTs
return their bins in their own order, so HIFIACC's natural order is read back into the same
slots: long blocks, bins 2j in the first half and 2j + 1 in the second; short blocks, the slot's
base-4 digits reversed. The input is shifted right until the sum of its |re| + |im| fits 31 bits
(no output can then overflow), and the output brought down to 24 bits, pvaacdec's own range; the
shift goes into the block exponent pvaacdec returns. The register sequence is shared with Vorbis
(`fw/hifi/haccfft.h`).

On the device: the FFT time of a 207 s track falls from 24.4 s to 10.9 s (transfers included);
within 1 LSB of pvaacdec on 87 % of the outputs, 9 LSB at most (on the block's peak taken as 16
bits); an AAC album (13 tracks) at 22.8-23.1 MHz of core B instead of 25 (-9 %), no timeout.
Test builds: `-DECHO_HWTEST` (both computed, pvaacdec's sound, `hwf:` log line) and
`-DECHO_FFTCMP` (HIFIACC's sound, compared with pvaacdec on a copy).

## The ALAC filter (verified on the device, used by our ALAC decoder)

Found in the stock ALAC module (overlay 79 of V3.9.0, `ETUDE.md` §113). Its decoder is FFmpeg's,
and the hardware replaces its `lpc_prediction`: the whole adaptive predictor, the coefficient
update after each sample included.

- The stock module uses it only when `lpc_quant` is below 12, for orders 1 to 30 (order 0 is a
  copy, order 31 the first-order case, both in software).
- The input buffer holds the first `order + 1` decoded samples (`out[0] = e[0]`, then
  `out[i] = sign_extend(out[i-1] + e[i], bps)`, computed in software), then the residuals.
- Sequence:
  1. `+0x0c` = `0x60000000 | n << 15`; `+0x04` |= `0x8000`;
  2. `+0x28` = `order << 16 | lpc_quant << 8 | bps << 24`;
  3. the coefficients, as 32-bit words, at `0x01069000`, in FFmpeg's array order;
  4. `+0x0c` = `0x60000000 | n << 15 | 3`;
  5. the same DMAC2 transfer as the FLAC filter (input to `+0x40`, output from `+0xa0`, in
     place);
  6. wait for bit 15 of `+0x08`, then `+0x04` &= ~`0x8000`.
- **On the device** (the bench below, `ETUDE.md` §116): exact against our software predictor
  for orders 3, 4, 8, 16 and 30, in 16-bit stereo (17 bits in the side channel), 24-bit 96 kHz
  and mono, with the coefficients in FFmpeg's order (reversed from the stream; in stream order
  every prediction differs). **Orders 1 and 2 are wrong**, from the first predicted sample on,
  as for the FLAC filter; the stock ALAC module does not avoid them (Apple's encoder only
  writes orders 4 and 8). 10 to 14 times faster than our predictor, DMA included (a 4,096-sample
  frame at order 8: about 0.4 ms against 4.4 ms on the bench).
- Not covered: `lpc_quant` of 12 and more (FFmpeg's encoder writes 5 to 9; kept in software, as
  the stock module does), blocks over 8,191 samples (the length field; the coefficients adapt
  along a block, so it cannot be split: kept in software).
- Our driver: `fw/hifi/alacacc.c`, for orders 3 to 30 and `lpc_quant` below 12.

## The APE filters (from the stock firmware, not verified yet)

Found in the stock APE module (overlay 80, `ETUDE.md` §113). The hardware runs the NN filters of
Monkey's Audio, one channel at a time; prediction and entropy decoding stay in software. Used
for compression levels above 1000.

- At the start of a stream: `+0x0c` = `0x1000 << 15`; `+0x20` = the configuration, then
  `+0x20` |= `0x200000` (probably clears the filter memories); `+0x10` = 3;
  `+0x00` = `0x01080108`; `+0x04` = `0x830083`; then `+0x0c` |= 3.
- Each block: `+0x0c` = `n << 15` (channel X) or `0x20000000 | n << 15` (channel Y); `+0x20` =
  the configuration; `+0x0c` |= 3; then the same in-place DMA transfer.
- `+0x20`: bit 31 for files of version 3980 and later, then per level: 2000 `0x040b0000`, 3000
  `0x100b0000`, 4000 `0x280ad000`, 5000 `0x440bdf00`. The nibbles at bits 16, 12 and 8 match
  the filters' shifts (11; 11; 10 and 13; 11, 13 and 15), and bits 26 to 30 the orders (16, 32,
  64, 256, 1,024); level 5000 lacks the bit expected for its 256-tap filter.
- The unstable memories at `+0x3000`-`+0x8fff` are probably the filters' state.

## The FIFOs without DMA

A routine left in the stock APE module (not called) feeds the FIFOs from the CPU: bit 0 of
`+0x08` means the input FIFO takes 8 more words; bit 16 means 8 output words are ready. Not
tried yet.

## How to test it yourself

This repository has a comparison bench, built only on request: `make EXTRA=-DECHO_HWTEST` in
`fw/`. With that image, every FLAC LPC subframe is computed both by the software decoder and by
HIFIACC, compared, and timed; the results go to the device log (`hwt:` lines) at the end of each
track, per order. The sound stays the software's. See `fw/hifi/hwtest_lpc.c`. The production
driver is `fw/hifi/hifiacc.c`.

The same image tests the ALAC filter (`fw/hifi/hwtest_alac.c`, `hwa:` lines): every prediction
of order 1 to 30 our ALAC decoder computes runs again on HIFIACC, with the coefficients
reversed (FFmpeg's order) and in stream order, and is compared. `fw/tools/mktestset.py --only
alacorder` makes ALAC files with the prediction order forced from 1 to 30.

The emulator `fw/tools/emu_fw.py` models the LPC filter and DMAC2 (`tools/emu_host.py`, class
`Accel`) and the ALAC filter (FFmpeg's adaptive predictor, exact on the device for orders 3 to
30). Orders 1 and 2, wrong on the hardware, make the model print `!! HIFIACC order` (a failure
in every emulator test). The two filters run in C (`tools/emu_accel.c`, built on first use into
`tools/__pycache__` and loaded through ctypes; in Python when there is no C compiler, or with
`EMU_NATIVE=0`), and the DMA moves a whole block per memory access.

It also models the FFT: the exact transform rounded to integers (the device's twiddles have about
14 bits, so results are close, not bit-exact), and the busy memory: while `+0x0c` still selects
the FFT, the data memory reads as `0x0badf00d`.

## Open questions

- The meaning of the configuration word `0x01080108`.
- The MAC (function 4, term count at bit 18 of `+0x30`, result at `+0x34`/`+0x38`): the FFT's
  sequence gives 0, and no stock module uses it.
- The APE filters: mapped from the stock firmware (above), to verify on the device.
- Driving the FIFOs directly from the CPU (above), which may suit small blocks.
- The MP3 accelerators SYNTH and Imdct36 are separate blocks: see `MP3ACC.md`.

Contributions and corrections are welcome.
