/* HIFIACC's LPC FIR for the FLAC decoder (ETUDE §66-67), with the register map of §1.5: the
 * coefficients in reverse order at 0x0106b000, the input (warm-up samples, then residuals) by
 * DMAC2 channel 1 into the FIFO at +0x40, the output by channel 0 from +0xa0. Exact against
 * dr_flac for orders 3 to 32 (orders 1 and 2 are wrong on the hardware: the stock FLAC module
 * keeps them in software too). Blocks over 8191 samples (13-bit length field) in passes, each
 * later one with the last outputs of the one before as its warm-up. Included by codec_flac.c. */
#include "haccdma.h"

#define HACC_SEG    8191u                       /* the length field: 13 bits */

static struct lli lli_in[5], lli_out[5];
static int hacc_ready;

/* One pass, in[0..n) -> out[0..n), n <= HACC_SEG (in == out allowed: the output follows the
 * input through the FIFO): 1 done, 0 timeout */
static int hacc_pass(const int32_t *in, int32_t *out, uint32_t n, uint32_t order, uint32_t shift)
{
    if (!hacc_ready) {
        REG(DMA + 0x398) = 1;                   /* DmaCfgReg: on */
        REG(HACC) = 0x01080108;                 /* as the stock module at start */
        hacc_ready = 1;
    }
    REG(DMA + 0x338) = 3;                       /* ClearTfr, ClearBlock, ClearErr */
    REG(DMA + 0x340) = 3;
    REG(DMA + 0x358) = 3;
    REG(HACC + 0x04) |= 0x8000;                 /* LPC mode */
    REG(HACC + 0x2c) = order << 16 | shift << 8;
    REG(HACC + 0x0c) = 0xa0000000u | n << 15;
    hacc_dma_block(lli_in, lli_out, in, out, n);
    REG(HACC + 0x0c) = 0xa0000000u | n << 15 | 3;   /* start */
    REG(DMA + 0x3a0) = 0x202;                   /* ChEnReg: channel 1, then 0 */
    REG(DMA + 0x3a0) = 0x101;
    uint32_t t0 = TIMER1_NOW();
    int ok = 0;
    /* 30 ms: HIFIACC runs at core B's clock, about one product per cycle: 8191 samples at
     * order 32 take some 5 ms at 50 MHz (0.85 timed out at 5 ms, ETUDE §67); a block of 8192
     * lasts 85 ms at 96 kHz */
    while (TIMER1_NOW() - t0 < 30 * 24000u)
        if ((REG(DMA + 0x2c0) & 1) && (REG(HACC + 0x08) & 0x8000)) {
            ok = 1;
            break;
        }
    if (!ok)
        REG(DMA + 0x3a0) = 0x300;               /* stop both channels */
    REG(HACC + 0x04) &= ~0x8000u;
    REG(HACC + 0x0c) = 0;
    return ok;
}

/* The LPC prediction of a block on HIFIACC, from in (warm-up then residuals) into out (in == out
 * allowed), order 3 to 32: 1 done, 0 timeout (out is then incomplete) */
static int hacc_lpc(int32_t *in, int32_t *out, uint32_t n, uint32_t order, uint32_t shift,
                    const int32_t *qlp)
{
    for (uint32_t k = 0; k < order; k++)        /* reverse order: COEF[k] weighs y[n-order+k] */
        REG(HACC_COEF + 4 * k) = (uint32_t)qlp[order - 1 - k];
    for (uint32_t k = order; k < 32; k++)
        REG(HACC_COEF + 4 * k) = 0;
    uint32_t s = 0;
    while (s < n) {
        uint32_t from = s ? s - order : 0, len = n - from < HACC_SEG ? n - from : HACC_SEG;
        const int32_t *src = in + from;
        if (s && in != out) {                   /* the warm-up of a later pass: outputs */
            for (uint32_t i = 0; i < order; i++)
                in[from + i] = out[from + i];
        }
        if (!hacc_pass(src, out + from, len, order, shift))
            return 0;
        s = from + len;
    }
    return 1;
}
