/* The sequential parts of the emulator's accelerator models (emu_host.py, class Accel), in C for
 * speed: built on first use into a shared library and called through ctypes; emu_host.py keeps
 * the same models in Python when no C compiler is there. Each output sample depends on the ones
 * before (and, for ALAC, the coefficients on each sample): numpy cannot vectorise them. */
#include <stdint.h>

/* HIFIACC's LPC filter (FLAC, ETUDE section 66): y[i] = x[i] for i < order, then
 * y[i] = x[i] + (sum_k coef[k] * y[i - order + k]) >> shift, the sum on 64 bits */
void flac_lpc(const int32_t *x, int32_t *y, int n, const int32_t *coef, int order, int shift)
{
    for (int i = 0; i < n && i < order; i++)
        y[i] = x[i];
    for (int i = order; i < n; i++) {
        int64_t acc = 0;
        for (int k = 0; k < order; k++)
            acc += (int64_t)coef[k] * y[i - order + k];
        y[i] = x[i] + (int32_t)(acc >> shift);
    }
}

static int sign_of(int32_t v)
{
    return (v > 0) - (v < 0);
}

/* HIFIACC's ALAC filter (ETUDE section 113): x the first order + 1 decoded samples then the
 * residuals; c the coefficients in FFmpeg's order (c[j] weighs the sample order - j before),
 * adapted after each sample; the sums on 32 bits as in FFmpeg's and Apple's decoders */
void alac_pred(const int32_t *x, int32_t *y, int n, int16_t *c, int order, int quant, int bps)
{
    for (int i = 0; i < n && i <= order; i++)
        y[i] = x[i];
    uint32_t half = quant ? 1u << (quant - 1) : 0;
    int sh = 32 - bps;
    for (int i = order + 1; i < n; i++) {
        int32_t d = y[i - order - 1];
        const int32_t *pred = y + i - order;
        uint32_t acc = 0;
        for (int j = 0; j < order; j++)
            acc += (uint32_t)((pred[j] - d) * c[j]);
        int32_t v = (int32_t)(acc + half) >> quant;
        y[i] = (int32_t)((uint32_t)(v + d + x[i]) << sh) >> sh;
        int32_t e = x[i];
        int es = sign_of(e);
        for (int j = 0; es && j < order && e * es > 0; j++) {
            int32_t dv = d - pred[j];
            int sg = sign_of(dv) * es;
            c[j] = (int16_t)(c[j] - sg);
            e -= ((dv * sg) >> quant) * (j + 1);
        }
    }
}
