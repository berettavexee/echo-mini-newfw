# HIFIACC: the RKNanoD audio accelerator

HIFIACC is a fixed-point accelerator inside the Rockchip RKNanoD, the SoC of the SNOWSKY ECHO
MINI. This page collects what this project established about it, so that other developers can
use it without repeating the work. The study behind it, in French, is `ETUDE.md` §1.5 (register
map), §66 (on-device comparison) and §67 (use in the FLAC decoder).

Status: the **LPC filter used by FLAC** is understood, verified bit-exact on the device and used
by this firmware since version 0.85. The other functions (FFT, MAC, ALAC, APE) are documented by
the TRM but their registers are not mapped yet.

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
| `0x01069000` | another memory, used by the stock ALAC module (not mapped yet) |

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

## How to test it yourself

This repository has a comparison bench, built only on request: `make EXTRA=-DECHO_HWTEST` in
`fw/`. With that image, every FLAC LPC subframe is computed both by the software decoder and by
HIFIACC, compared, and timed; the results go to the device log (`hwt:` lines) at the end of each
track, per order. The sound stays the software's. See `fw/hifi/hwtest_lpc.c`. The production
driver is `fw/hifi/hifiacc.c`.

The emulator `fw/tools/emu_fw.py` models the LPC filter and DMAC2 (`fw/tools/emu_host.py`, class
`Accel`); the model is exact for orders 3 to 32 but, unlike the hardware, also for orders 1
and 2.

## Open questions

- The meaning of the configuration word `0x01080108`.
- The registers of the FFT/IFFT, the MAC, the ALAC filter (variable coefficients, memory at
  `0x01069000`) and the APE decoder.
- Driving the FIFOs directly from the CPU (the TRM allows it), which may suit small blocks.
- The MP3 accelerators **SYNTH** and **Imdct36** are separate blocks (TRM chapters 27 and 28,
  with memory maps); not studied yet.

Contributions and corrections are welcome.
