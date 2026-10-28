/* The FFTs of pvaacdec's IMDCT on HIFIACC (ETUDE §76): mix_radix_fft (long blocks, 512 points)
 * and fft_rx4_short (short blocks, 64 points), called from imdct_fxp.cpp. pvaacdec's pre- and
 * post-rotations stay its own; only the FFT moves, with the same contract: data[] complex
 * (re, im) in place, *max the peak in and out (|x| OR-ed as (x >> 31) ^ x), returns the exponent
 * e (output = the DFT x 2^-e). pvaacdec's outputs are not in natural order, and HIFIACC's are
 * read back into the same slots: long blocks, bins 2j in the first half and 2j + 1 in the second
 * (its radix-2 split), short blocks, the slot's base-4 digits reversed (radix 4); checked on the
 * PC against both functions. The input goes in shifted right just enough that the unscaled FFT
 * cannot overflow 32 bits: no output part exceeds the sum of the |re| + |im| of the input (a
 * bound much tighter than points x peak on real blocks, so fewer bits are lost; 0.92hwt1 used
 * the peak: up to 15 LSB of difference, §76). The outputs are then brought down to 24 bits, the
 * range of pvaacdec's own FFT, the only one the rest of its IMDCT was made and tested for.
 * echo_aac_fft: -1 = not done (another size, a timeout), pvaacdec's FFT then, and
 * echo_aac_fft_check after it. -DECHO_NO_HWFFT leaves pvaacdec alone; -DECHO_HWTEST computes
 * both and compares them, the sound from pvaacdec (ctl->hwf); -DECHO_FFTCMP plays HIFIACC's
 * and compares it with pvaacdec's on a copy (imdct_fxp.cpp). Included by codec_aac.c. */
#if !defined(ECHO_NO_HWFFT) || defined(ECHO_HWTEST)
#include "haccfft.h"
#include "hooks.h"

static uint32_t slot_of(uint32_t k, uint32_t bits)
{
    if (bits == 9)
        return k & 1 ? 256 + (k >> 1) : k >> 1;
    return (k & 3) << 4 | (k & 0xc) | (k >> 4 & 3);
}

/* HIFIACC's FFT of data into out (data == out allowed): the exponent, or -1 */
static int aac_fft_hw(const int32_t *data, int bits, int32_t *max, int32_t *out)
{
    if ((bits != 9 && bits != 6) || fft_timeouts >= 3)
        return -1;
    uint32_t n = 1u << bits;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < 2 * n; i++)
        sum += (uint32_t)((data[i] >> 31) ^ data[i]) + 1u;
    int s = 0;
    while (sum >> s > 0x7fffffffu)
        s++;
    volatile int32_t *re = FFT_RE_MEM, *im = FFT_IM_MEM;
    for (uint32_t j = 0; j < n; j++) {
        uint32_t i = rbit(j) >> (32 - bits);
        re[i] = data[2 * j] >> s;
        im[i] = data[2 * j + 1] >> s;
    }
    if (!hacc_fft((uint32_t)bits))
        return -1;
    int32_t m = 0;
    for (uint32_t k = 0; k < n; k++) {
        int32_t a = re[k], b = im[k];
        uint32_t j = slot_of(k, (uint32_t)bits);
        out[2 * j] = a;
        out[2 * j + 1] = b;
        m |= (a >> 31) ^ a;
        m |= (b >> 31) ^ b;
    }
    /* down to 24 bits, as pvaacdec's own FFT */
    uint32_t k = 0;
    while ((uint32_t)m >> k >= 1u << 24)
        k++;
    if (k) {
        for (uint32_t i = 0; i < 2 * n; i++)
            out[i] >>= k;
        m >>= k;
    }
    *max = m;
    return s + (int)k;
}

#ifdef ECHO_FFTCMP
/* Test build (0.92 silence, ETUDE §76): the sound from HIFIACC; pvaacdec's FFT of a copy of the
 * input compared with it, in LSB of the block's peak taken as 16 bits; the worst block kept */
static int64_t scaled_c(int32_t v, int e)
{
    return e >= 0 ? (int64_t)v << e : (int64_t)v >> -e;
}

void echo_aac_fft_cmp(const int32_t *hw, int e_hw, const int32_t *sw, int e_sw, int bits)
{
    uint32_t n = 2u << bits, worst = 0, wi = 0;
    int64_t peak = 1;
    int32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        int64_t v = scaled_c(sw[i], e_sw);
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
        m |= (hw[i] >> 31) ^ hw[i];
    }
    int64_t lsb = peak >> 15 ? peak >> 15 : 1;
    ctl->hwf.magic = HWT_MAGIC;
    ctl->hwf.blocks++;
    if (bits == 6)
        ctl->hwf.short_blocks++;
    for (uint32_t i = 0; i < n; i++) {
        int64_t d = scaled_c(hw[i], e_hw) - scaled_c(sw[i], e_sw);
        if (d < 0)
            d = -d;
        uint32_t err = (uint32_t)((d + lsb / 2) / lsb);
        ctl->hwf.samples++;
        if (!err)
            ctl->hwf.exact++;
        else if (err == 1)
            ctl->hwf.within1++;
        if (err > worst) {
            worst = err;
            wi = i;
        }
    }
    if (worst > ctl->hwf.max) {
        ctl->hwf.max = worst;
        ctl->hwf.worst_at = ctl->fft_blocks;
        ctl->hwf.worst_info = (uint32_t)e_hw | (uint32_t)e_sw << 8 | (uint32_t)bits << 16;
        ctl->hwf.worst_m = (uint32_t)m;
        ctl->hwf.worst_i = wi;
    }
}
#endif

#ifndef ECHO_HWTEST
int echo_aac_fft(int32_t *data, int bits, int32_t *max)
{
    int e = aac_fft_hw(data, bits, max, data);
    if (e >= 0)
        ctl->fft_blocks++;
    return e;
}

void echo_aac_fft_check(const int32_t *data, int bits, int e)
{
    (void)data;
    (void)bits;
    (void)e;
    ctl->fft_soft++;
}
#else
/* Test build: HIFIACC's result kept aside and timed, pvaacdec computes the sound, then both are
 * compared on the DFT's own scale, in LSB of the block's peak taken as 16 bits */
static int64_t scaled(int32_t v, int e)          /* v x 2^e */
{
    return e >= 0 ? (int64_t)v << e : (int64_t)v >> -e;
}

static int32_t hw_out[1024];
static int hw_e = -1;
static uint32_t t_sw;

int echo_aac_fft(int32_t *data, int bits, int32_t *max)
{
    int32_t m = *max;
    uint32_t t0 = TIMER1_NOW();
    hw_e = aac_fft_hw(data, bits, &m, hw_out);
    uint32_t t1 = TIMER1_NOW();
    if (hw_e >= 0)
        ctl->hwf.hw_ticks += t1 - t0;
    else if (bits == 9 || bits == 6)
        ctl->hwf.timeouts++;
    t_sw = TIMER1_NOW();
    return -1;
}

void echo_aac_fft_check(const int32_t *data, int bits, int e)
{
    uint32_t t = TIMER1_NOW() - t_sw;
    if (hw_e < 0)
        return;
    uint32_t n = 2u << bits;
    int64_t peak = 1;
    for (uint32_t i = 0; i < n; i++) {
        int64_t v = scaled(data[i], e);
        if (v < 0)
            v = -v;
        if (v > peak)
            peak = v;
    }
    int64_t lsb = peak >> 15 ? peak >> 15 : 1;
    ctl->hwf.magic = HWT_MAGIC;
    ctl->hwf.sw_ticks += t;
    ctl->hwf.blocks++;
    if (bits == 6)
        ctl->hwf.short_blocks++;
    for (uint32_t i = 0; i < n; i++) {
        int64_t d = scaled(hw_out[i], hw_e) - scaled(data[i], e);
        if (d < 0)
            d = -d;
        uint32_t err = (uint32_t)((d + lsb / 2) / lsb);     /* rounded, in LSB */
        ctl->hwf.samples++;
        if (!err)
            ctl->hwf.exact++;
        else if (err == 1)
            ctl->hwf.within1++;
        if (err > ctl->hwf.max)
            ctl->hwf.max = err;
    }
    hw_e = -1;
}
#endif
#endif
