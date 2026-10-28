/* The IMDCT of Tremor (mdct_backward) through HIFIACC's FFT (ETUDE §73-74), for the Vorbis
 * block sizes 256 and 2048: a pre-rotation of the n/2 coefficients into n/4 complex values,
 * written in bit-reversed order into the FFT memory (the hardware wants its input so), the
 * FFT of n/4 points, then a post-rotation read back in natural order and unfolded into the
 * n/2 outputs in Tremor's own order (mdct_unroll_lap reads them):
 *   d = w * FFT(w * (X[2m] + i X[n/2-1-2m])),  w[m] = exp(-2 i pi (m + 1/8) / n)
 *   out[4q] = -Im d[q], out[4q+1] = -Re d[q], out[4q+2] = Re d[n/4-1-q], out[4q+3] = Im d[n/4-1-q]
 * Measured on real blocks against Tremor (ETUDE §74): within 1 LSB of the 16-bit output for
 * blocks of 256, 4 LSB at most (0.06 on average) for 2048: the hardware's twiddles. Other sizes,
 * and a timeout, stay in software (Tremor's own code after the hook in mdct.c).
 * On by default since 0.88 (the sound from HIFIACC); -DECHO_NO_HWFFT leaves Tremor alone, and
 * -DECHO_HWTEST computes both and compares them, the sound from Tremor (ctl->hwf). Included by
 * codec_vorbis.c. */
#if !defined(ECHO_NO_HWFFT) || defined(ECHO_HWTEST)
#include "fft_tw.h"

#ifndef REG
#define REG(a) (*(volatile uint32_t *)(a))
#endif

#define HACC        0x01060000u
#define FFT_IM      0x01061000u
#define FFT_RE      0x01062000u
#define FFT_NOW()   (~REG(0x400e0028u))         /* TIMER1, 24 MHz */
#define FFT_WAIT    (24000u * 3)                /* 3 ms: some 0.1 ms at 24 MHz (to measure) */

static uint32_t fft_timeouts;

static inline uint32_t rbit(uint32_t x)
{
    uint32_t r;
    __asm__("rbit %0, %1" : "=r"(r) : "r"(x));
    return r;
}

/* out[0..n/2) from in[0..n/2), on HIFIACC (in == out allowed): 1 done, 0 not (size, timeout) */
static int fft_imdct(int n, const int32_t *in, int32_t *out)
{
    const int32_t (*w)[2];
    uint32_t size, bits, n4 = (uint32_t)n / 4;
    if (n == 2048) {
        w = fft_tw2048;
        size = 3;                               /* 512 points */
        bits = 9;
    } else if (n == 256) {
        w = fft_tw256;
        size = 0;                               /* 64 points */
        bits = 6;
    } else
        return 0;
    if (fft_timeouts >= 3)
        return 0;
    volatile int32_t *re = (volatile int32_t *)FFT_RE, *im = (volatile int32_t *)FFT_IM;
    for (uint32_t m = 0; m < n4; m++) {
        int64_t a = in[2 * m], b = in[n / 2 - 1 - 2 * m], c = w[m][0], s = w[m][1];
        uint32_t j = rbit(m) >> (32 - bits);
        re[j] = (int32_t)((a * c + b * s) >> 31);
        im[j] = (int32_t)((b * c - a * s) >> 31);
    }
    REG(HACC + 0x04) |= 0x8000;                 /* "done" in +0x08 bit 15 */
    REG(HACC + 0x24) = size << 29 | 1u << 27;   /* FFT without scaling, complex input */
    REG(HACC + 0x0c) = 0x40000000;              /* function 2: FFT */
    REG(HACC + 0x0c) = 0x40000003;
    uint32_t t0 = FFT_NOW();
    while (!(REG(HACC + 0x08) & 0x8000))
        if (FFT_NOW() - t0 > FFT_WAIT)
            break;
    int done = (REG(HACC + 0x08) & 0x8000) != 0;
    REG(HACC + 0x04) &= ~0x8000u;
    REG(HACC + 0x0c) = 0;                       /* the memory back to the bus */
    if (!done) {
        fft_timeouts++;
        return 0;
    }
    for (uint32_t q = 0; q < n4 / 2; q++) {
        uint32_t k = n4 - 1 - q;
        int64_t fr = re[q], fi = im[q], c = w[q][0], s = w[q][1];
        int64_t gr = re[k], gi = im[k], c2 = w[k][0], s2 = w[k][1];
        out[4 * q] = -(int32_t)((fi * c - fr * s) >> 31);
        out[4 * q + 1] = -(int32_t)((fr * c + fi * s) >> 31);
        out[4 * q + 2] = (int32_t)((gr * c2 + gi * s2) >> 31);
        out[4 * q + 3] = (int32_t)((gi * c2 - gr * s2) >> 31);
    }
    return 1;
}

#ifndef ECHO_HWTEST
/* Called by mdct_backward first: 1 = done here, Tremor returns */
int echo_fft_imdct(int n, int32_t *in)
{
    if (fft_imdct(n, in, in)) {
        ctl->fft_blocks++;
        return 1;
    }
    if (n == 256 || n == 2048)
        ctl->fft_soft++;
    return 0;
}

void echo_fft_check(int n, const int32_t *in)
{
    (void)n;
    (void)in;
}
#else
/* Test build: HIFIACC's result kept aside and timed, Tremor computes the sound, then both are
 * compared on the 16-bit output scale (Tremor's CLIP_TO_15(x >> 9)) */
static int32_t fft_hw_out[1024];
static int fft_hw_ok;
static uint32_t fft_t_sw;

int echo_fft_imdct(int n, int32_t *in)
{
    uint32_t t0 = FFT_NOW();
    fft_hw_ok = fft_imdct(n, in, fft_hw_out);
    uint32_t t1 = FFT_NOW();
    if (fft_hw_ok)
        ctl->hwf.hw_ticks += t1 - t0;
    else if (n == 256 || n == 2048)
        ctl->hwf.timeouts++;
    fft_t_sw = FFT_NOW();
    return 0;
}

void echo_fft_check(int n, const int32_t *in)
{
    uint32_t t = FFT_NOW() - fft_t_sw;
    if (!fft_hw_ok)
        return;
    fft_hw_ok = 0;
    ctl->hwf.magic = HWT_MAGIC;
    ctl->hwf.sw_ticks += t;
    ctl->hwf.blocks++;
    if (n == 256)
        ctl->hwf.short_blocks++;
    for (int i = 0; i < n / 2; i++) {
        int32_t d = (in[i] >> 9) - (fft_hw_out[i] >> 9);
        uint32_t e = (uint32_t)(d < 0 ? -d : d);
        ctl->hwf.samples++;
        if (!e)
            ctl->hwf.exact++;
        else if (e == 1)
            ctl->hwf.within1++;
        if (e > ctl->hwf.max)
            ctl->hwf.max = e;
    }
}
#endif
#endif
