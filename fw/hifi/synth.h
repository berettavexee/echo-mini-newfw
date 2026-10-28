/* The SYNTH accelerator (ETUDE §68-71, docs/MP3ACC.md), shared by mp3synth.c (MP3) and
 * mpcsynth.c (Musepack): the MPEG polyphase synthesis, 32 subband samples of one channel in, 32
 * 16-bit PCM samples out, its history (SRAM1) kept per channel. Per slot: syn_begin(ch), the 32
 * inputs to SYN_IO(sb) (the computation starts by itself), syn_wait(), the outputs from
 * SYN_IO(k) (low 16 bits). Included by the codec's own file. */
#ifndef SYNTH_H
#define SYNTH_H

#include "accel.h"

#define SYN_IO(i)   REG(SYN + 4 * (i))

static int syn_on, syn_timeouts;                /* 3 timeouts: software for the rest */
static uint32_t syn_phase[2];

/* The history and phases to 0 (the start of a stream) */
static void syn_clear(void)
{
    REG(SYN + 0x800) = 1;                       /* on, as the stock module 73 */
    REG(SYN + 0x810) = 0;
    REG(SYN + 0x80c) = 0x11;
    for (uint32_t i = 0; i < 1024; i++)
        REG(SYN_HIST + 4 * i) = 0;
    syn_phase[0] = syn_phase[1] = 0;
    syn_on = 1;
}

/* Before the inputs of a slot of channel ch: its phase, moved on by 1 (mod 16) per slot */
static inline void syn_begin(uint32_t ch)
{
    REG(SYN + 0x804) = ch | (15 - syn_phase[ch]) << 1;
    syn_phase[ch] = (syn_phase[ch] + 1) & 15;
}

/* After the inputs: 1 done (the outputs in SYN_IO), 0 timeout (counted) */
static int syn_wait(void)
{
    uint32_t t0 = TIMER1_NOW();
    int done = 0;
    while (TIMER1_NOW() - t0 < 200 * 24u)       /* a slot takes a few microseconds */
        if (REG(SYN + 0x80c) != 0) {
            done = 1;
            break;
        }
    REG(SYN + 0x80c) = 0x11;
    REG(SYN + 0x810) = 0;
    if (!done)
        syn_timeouts++;
    return done;
}

#endif
