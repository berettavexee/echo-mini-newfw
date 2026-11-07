/* ALAC's adaptive predictor on HIFIACC (ETUDE §113, §116), with the sequence of the stock module
 * 79: function 3 in +0x0c, LPC mode (+0x04 bit 15), order, lpc_quant and sample bits in +0x28,
 * the coefficients at 0x01069000 in FFmpeg's order (reversed from the stream), the FLAC's DMA,
 * in place. Exact against our predictor (alacdec.c) for orders 3 to 30 on the device (bench
 * §114-116); orders 1 and 2 are wrong on the hardware, as for the FLAC (§66). The stock module
 * keeps lpc_quant >= 12 in software; a block over 8191 samples (13-bit length field) cannot be
 * split (the coefficients adapt along it). Those, and the rest of a track after 3 timeouts, stay
 * in software. Included by codec_alac.c; called by alacdec.c's unpc_block(). */
#include "haccdma.h"

#define ALACC_MAX_N 8191u                       /* the length field: 13 bits */

static uint32_t alacc_timeouts;

static void alacc_track(void)                   /* codec_open: the counters of a track */
{
    ctl->alac_hw = ctl->alac_low = ctl->alac_quant = ctl->alac_long = ctl->alac_soft = 0;
    alacc_timeouts = 0;
}

#if defined(ECHO_NO_HWALAC) || defined(ECHO_HWTEST)
/* (the test build compares, hwtest_alac.c; its sound stays ours) */
int echo_alac_hw(const int32_t *pc, int32_t *out, uint32_t num, const int16_t *coefs,
                 uint32_t act, uint32_t chanbits, uint32_t denshift)
{
    (void)pc, (void)out, (void)num, (void)coefs, (void)act, (void)chanbits, (void)denshift;
    return 0;
}
#else
static struct lli alacc_in[4], alacc_out[4];
static int alacc_ready;

/* buf: the order + 1 first samples decoded, then the residuals; in place: 1 done, 0 timeout */
static int alacc_run(int32_t *buf, uint32_t num, const int16_t *coefs, uint32_t act,
                     uint32_t chanbits, uint32_t denshift)
{
    if (!alacc_ready) {
        REG(DMA + 0x398) = 1;                   /* DmaCfgReg: on */
        REG(HACC) = 0x01080108;                 /* as the stock modules at start */
        alacc_ready = 1;
    }
    REG(DMA + 0x338) = 3;                       /* ClearTfr, ClearBlock, ClearErr */
    REG(DMA + 0x340) = 3;
    REG(DMA + 0x358) = 3;
    /* the stock module 79's order: the function and length, the mode, the configuration, the
     * coefficients, the start */
    REG(HACC + 0x0c) = 0x60000000u | num << 15;
    REG(HACC + 0x04) |= 0x8000;
    REG(HACC + 0x28) = act << 16 | denshift << 8 | chanbits << 24;
    for (uint32_t k = 0; k < act; k++)
        REG(HACC_ACOEF + 4 * k) = (uint32_t)(int32_t)coefs[act - 1 - k];
    hacc_dma_block(alacc_in, alacc_out, buf, buf, num);
    REG(HACC + 0x0c) = 0x60000000u | num << 15 | 3;     /* start */
    REG(DMA + 0x3a0) = 0x202;                   /* ChEnReg: channel 1, then 0 */
    REG(DMA + 0x3a0) = 0x101;
    uint32_t t0 = TIMER1_NOW();
    int ok = 0;
    while (TIMER1_NOW() - t0 < 30 * 24000u)     /* 30 ms, as hifiacc.c (a block: ~1 ms) */
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

/* alacdec.c's predictor of order act (1 to 30) on HIFIACC: 1 done (out complete), 0 for the
 * software (pc untouched: pc != out) */
int echo_alac_hw(const int32_t *pc, int32_t *out, uint32_t num, const int16_t *coefs,
                 uint32_t act, uint32_t chanbits, uint32_t denshift)
{
    if (act < 3) {
        ctl->alac_low++;
        return 0;
    }
    if (denshift >= 12) {
        ctl->alac_quant++;
        return 0;
    }
    if (num > ALACC_MAX_N) {
        ctl->alac_long++;
        return 0;
    }
    if (alacc_timeouts >= 3 || pc == out) {
        ctl->alac_soft++;
        return 0;
    }
    uint32_t cs = 32 - chanbits;
    out[0] = pc[0];
    for (uint32_t j = 1; j <= act; j++) {       /* the warm-up, as the stock module (0x4608) */
        int32_t d = pc[j] + out[j - 1];
        out[j] = (d << cs) >> cs;
    }
    for (uint32_t j = act + 1; j < num; j++)
        out[j] = pc[j];
    if (!alacc_run(out, num, coefs, act, chanbits, denshift)) {
        alacc_timeouts++;
        ctl->alac_soft++;
        return 0;
    }
    ctl->alac_hw++;
    return 1;
}
#endif
