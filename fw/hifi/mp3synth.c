/* The MP3 synthesis on the SYNTH accelerator (ETUDE §68-71) instead of pvmp3dec's
 * pvmp3_poly_phase_synthesis: per slot of 32 subband samples and per channel, +0x804 = channel |
 * (15 - phase) << 1, the 32 inputs at 0x60050000 (<< 4: SYNTH takes them 16 times larger than
 * pvmp3dec keeps them), wait for +0x80c, acknowledge (+0x80c = 0x11, +0x810 = 0), read the 32
 * PCM samples back from the same SRAM; the phase moves on by 1 (mod 16) per slot. The history
 * (SRAM1, 0x60051000) is cleared at the start and whenever pvmp3dec clears its own. Within 2 LSB
 * of pvmp3dec, about 42 % less core B load (ETUDE §70). Included by codec_mp3.c. */
#include "synth.h"

/* The history and phases to 0 (start, and pvmp3dec's own reset after a damaged frame) */
void echo_synth_reset(void)
{
    syn_clear();
}

/* One granule of one channel: 18 slots (work: subband sb, slot s at sb * 18 + s) into out
 * (stride nch). 1 done, 0: SYNTH is off for the track (3 timeouts), the caller uses software. */
int echo_synth_granule(const int32_t *work, int16_t *out, int nch, int ch)
{
    if (syn_timeouts >= 3)
        return 0;
    if (!syn_on)
        syn_clear();
    for (int s = 0; s < 18; s++) {
        syn_begin((uint32_t)ch);
        for (int sb = 0; sb < 32; sb++)
            SYN_IO(sb) = (uint32_t)work[sb * 18 + s] << 4;
        if (!syn_wait()) {
            ctl->synth_soft++;
            return 0;
        }
        int16_t *o = out + s * 32 * nch;
        for (int k = 0; k < 32; k++)
            o[k * nch] = (int16_t)SYN_IO(k);
    }
    ctl->synth_gran++;
    return 1;
}
