/* The MP3 synthesis on the SYNTH accelerator (ETUDE §68-71) instead of pvmp3dec's
 * pvmp3_poly_phase_synthesis: per slot of 32 subband samples and per channel, +0x804 = channel |
 * (15 - phase) << 1, the 32 inputs at 0x60050000 (<< 4: SYNTH takes them 16 times larger than
 * pvmp3dec keeps them), wait for +0x80c, acknowledge (+0x80c = 0x11, +0x810 = 0), read the 32
 * PCM samples back from the same SRAM; the phase moves on by 1 (mod 16) per slot. The history
 * (SRAM1, 0x60051000) is cleared at the start and whenever pvmp3dec clears its own. Within 2 LSB
 * of pvmp3dec, about 42 % less core B load (ETUDE §70). Included by codec_mp3.c. */
#ifndef REG
#define REG(a) (*(volatile uint32_t *)(a))
#endif
#define SYN         0x60050000u
#define SYN_HIST    0x60051000u
#define SYN_NOW()   (~REG(0x400e0028u))          /* TIMER1, 24 MHz */

static int syn_on, syn_timeouts;
static uint32_t syn_phase[2];

/* The history and phases to 0 (start, and pvmp3dec's own reset after a damaged frame) */
void echo_synth_reset(void)
{
    REG(SYN + 0x800) = 1;                       /* on, as the stock module 73 */
    REG(SYN + 0x810) = 0;
    REG(SYN + 0x80c) = 0x11;
    for (uint32_t i = 0; i < 1024; i++)
        REG(SYN_HIST + 4 * i) = 0;
    syn_phase[0] = syn_phase[1] = 0;
    syn_on = 1;
}

/* One granule of one channel: 18 slots (work: subband sb, slot s at sb * 18 + s) into out
 * (stride nch). 1 done, 0: SYNTH is off for the track (3 timeouts), the caller uses software. */
int echo_synth_granule(const int32_t *work, int16_t *out, int nch, int ch)
{
    if (syn_timeouts >= 3)
        return 0;
    if (!syn_on)
        echo_synth_reset();
    for (int s = 0; s < 18; s++) {
        REG(SYN + 0x804) = (uint32_t)ch | (15 - syn_phase[ch]) << 1;
        for (int sb = 0; sb < 32; sb++)
            REG(SYN + 4 * sb) = (uint32_t)work[sb * 18 + s] << 4;
        syn_phase[ch] = (syn_phase[ch] + 1) & 15;
        uint32_t t0 = SYN_NOW();
        int done = 0;
        while (SYN_NOW() - t0 < 200 * 24u)      /* a slot takes a few microseconds */
            if (REG(SYN + 0x80c) != 0) {
                done = 1;
                break;
            }
        REG(SYN + 0x80c) = 0x11;
        REG(SYN + 0x810) = 0;
        if (!done) {
            syn_timeouts++;
            ctl->synth_soft++;
            return 0;
        }
        int16_t *o = out + s * 32 * nch;
        for (int k = 0; k < 32; k++)
            o[k * nch] = (int16_t)REG(SYN + 4 * k);
    }
    ctl->synth_gran++;
    return 1;
}
