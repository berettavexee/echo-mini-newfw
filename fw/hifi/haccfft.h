/* HIFIACC's FFT (ETUDE §73-74, docs/HIFIACC.md), shared by hififft.c (Vorbis) and aacfft.c
 * (AAC): 2^bits complex points (bits 6 to 9) in its own memory, the input written in
 * bit-reversed order, the output read back in natural order; no scaling. Included by the
 * codec's own file. */
#ifndef HACCFFT_H
#define HACCFFT_H

#include "accel.h"

#define FFT_WAIT    (24000u * 3)                /* 3 ms: some 0.1 ms at 24 MHz (to measure) */
#define FFT_RE_MEM  ((volatile int32_t *)FFT_RE)
#define FFT_IM_MEM  ((volatile int32_t *)FFT_IM)

static uint32_t fft_timeouts;                   /* 3 of them: software for the rest */

static inline uint32_t rbit(uint32_t x)
{
    uint32_t r;
    __asm__("rbit %0, %1" : "=r"(r) : "r"(x));
    return r;
}

/* The FFT of what is in FFT_RE_MEM / FFT_IM_MEM: 1 done, 0 timeout (counted) */
static int hacc_fft(uint32_t bits)
{
    REG(HACC + 0x04) |= 0x8000;                 /* "done" in +0x08 bit 15 */
    REG(HACC + 0x24) = (bits - 6) << 29 | 1u << 27;     /* size, no scaling, complex input */
    REG(HACC + 0x0c) = 0x40000000;              /* function 2: FFT */
    REG(HACC + 0x0c) = 0x40000003;
    uint32_t t0 = TIMER1_NOW();
    while (!(REG(HACC + 0x08) & 0x8000))
        if (TIMER1_NOW() - t0 > FFT_WAIT)
            break;
    int done = (REG(HACC + 0x08) & 0x8000) != 0;
    REG(HACC + 0x04) &= ~0x8000u;
    REG(HACC + 0x0c) = 0;                       /* the memory back to the bus */
    if (!done)
        fft_timeouts++;
    return done;
}

#endif
