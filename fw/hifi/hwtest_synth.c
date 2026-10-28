/* Test build only (make EXTRA=-DECHO_HWTEST, ETUDE §69): every synthesis slot pvmp3dec computes
 * (32 subband samples -> 32 PCM samples, per channel) is computed again on the MP3 SYNTH
 * accelerator with the register use of the stock module 73 (ETUDE §68), and compared. The input
 * scale is not known: each track tries another input shift (hws.trial), and the log gives the
 * amplitude ratio, the sign and the errors in direct and reversed order. Included by
 * codec_mp3.c; called from pvmp3_framedecoder.cpp after pvmp3_poly_phase_synthesis(). */
#define SYN         0x60050000u
#define SYN_HIST    0x60051000u
#define SYN_NOW()   (~REG(0x400e0028u))          /* TIMER1, 24 MHz */

/* SYNTH: input x << 4 (HS2: 2 LSB at most against pvmp3dec, ETUDE §69). IMDCT36 (HS3): per
 * track, +0x800 = 3 or 1 and an input shift of 4 bits left or none */
static const uint8_t im_modes[4] = { 3, 1, 3, 1 };
static const int8_t im_shifts[4] = { -4, -4, 0, 0 };
#include "hwtest_imdct_tab.h"
#define IMD         0x60010000u
static int syn_ready;
static uint32_t syn_phase[2];

static void syn_reset(void)
{
    volatile __typeof__(ctl->hws) *h = &ctl->hws;
    uint32_t trial = h->trial;                  /* kept by core A, which moves it on */
    memset((void *)h, 0, sizeof *h);
    h->magic = HWT_MAGIC;
    h->trial = trial;
    h->shift = -4;
    h->im_mode = im_modes[trial & 3];
    h->im_shift = im_shifts[trial & 3];
    REG(IMD + 0x80c) = 0;
    REG(IMD + 0x800) = h->im_mode;              /* module 73: 3 at start */
    REG(SYN + 0x800) = 1;                       /* on, as module 73 at start */
    REG(SYN + 0x810) = 0;
    REG(SYN + 0x80c) = 0x11;
    for (uint32_t i = 0; i < 1024; i++)         /* the history (SRAM1), as module 73 at reset */
        REG(SYN_HIST + 4 * i) = 0;
    syn_phase[0] = syn_phase[1] = 0;
    syn_ready = 1;
}

/* work: pvmp3dec's work_buf_int32 (subband sb, slot s at sb * 18 + s); out: this channel's
 * first PCM sample, stride nch; 18 slots of 32 samples */
/* ECHO_HWSYN (ETUDE §71): the synthesis of one granule and channel on SYNTH, into the PCM
 * output (stride nch), instead of pvmp3_poly_phase_synthesis */
void echo_synth_play(const int32_t *work, int16_t *out, int nch, int ch)
{
    if (!syn_ready)
        syn_reset();
    volatile __typeof__(ctl->hws) *h = &ctl->hws;
    uint32_t t0 = SYN_NOW();
    for (int s = 0; s < 18; s++) {
        REG(SYN + 0x804) = (uint32_t)ch | (15 - syn_phase[ch]) << 1;
        for (int sb = 0; sb < 32; sb++)
            REG(SYN + 4 * sb) = (uint32_t)work[sb * 18 + s] << 4;
        syn_phase[ch] = (syn_phase[ch] + 1) & 15;
        uint32_t t1 = SYN_NOW();
        int polled = 0;
        while (SYN_NOW() - t1 < 200 * 24u)
            if (REG(SYN + 0x80c) != 0) {
                polled = 1;
                break;
            }
        h->p_polled += polled;
        h->p_timeouts += !polled;
        REG(SYN + 0x80c) = 0x11;
        REG(SYN + 0x810) = 0;
        int16_t *o = out + s * 32 * nch;
        for (int k = 0; k < 32; k++)
            o[k * nch] = (int16_t)REG(SYN + 4 * k);
    }
    h->p_hw_ticks += SYN_NOW() - t0;
}

/* The comparison of a granule's output with the software's (both channels interleaved) */
void echo_synth_cmp(const int16_t *hw, const int16_t *sw, int n)
{
    volatile __typeof__(ctl->hws) *h = &ctl->hws;
    for (int i = 0; i < n; i++) {
        int32_t d = hw[i] - sw[i];
        uint32_t a = d < 0 ? -d : d;
        h->p_exact += a == 0;
        h->p_within1 += a <= 1;
        if (a > h->p_max)
            h->p_max = a;
    }
    h->p_cmp += n;
}

uint32_t echo_now(void)
{
    return SYN_NOW();
}

/* One IMDCT of 36 points on IMDCT36 against the exact one; x: the 18 inputs pvmp3_mdct_18 got;
 * sw: its time on a copy */
void echo_imdct_hook(const int32_t *x, uint32_t sw)
{
    if (!syn_ready)
        syn_reset();
    volatile __typeof__(ctl->hws) *h = &ctl->hws;
    int32_t sh = h->im_shift;
    uint32_t t0 = SYN_NOW();
    for (int k = 0; k < 18; k++)
        REG(IMD + 4 * k) = sh >= 0 ? (uint32_t)(x[k] >> sh) : (uint32_t)x[k] << -sh;
    uint32_t t1 = SYN_NOW(), st = 0;
    int polled = 0;
    while (SYN_NOW() - t1 < 200 * 24u)
        if ((st = REG(IMD + 0x80c)) != 0) {
            polled = 1;
            break;
        }
    int32_t hw[36];
    for (int i = 0; i < 36; i++)
        hw[i] = (int32_t)REG(IMD + 0x48 + 4 * i);
    REG(IMD + 0x80c) = 0;                       /* acknowledge, as module 73 */
    h->im_hw_ticks += SYN_NOW() - t0;
    h->im_sw_ticks += sw;
    h->im_polled += polled;
    h->im_timeouts += !polled;
    for (int i = 0; i < 36; i++) {
        int64_t acc = 0;
        for (int k = 0; k < 18; k++)
            acc += (int64_t)x[k] * imdct36_cos[i][k];
        int32_t ref = (int32_t)(acc >> 31);
        for (int e = 0; e < 17; e++) {          /* ref * 2^(e - 8) */
            int64_t r = e >= 8 ? (int64_t)ref << (e - 8) : ref >> (8 - e);
            int64_t d = hw[i] - r;
            h->im_exact[e] += d == 0;
            h->im_within1[e] += d >= -1 && d <= 1;
        }
        h->im_sum_ref += ref < 0 ? -(int64_t)ref : ref;
        h->im_sum_hw += hw[i] < 0 ? -(int64_t)hw[i] : hw[i];
        h->im_dot += ((int64_t)ref * hw[i]) >> 16;
        if (h->im_calls == 0 && i < 3) {
            h->im_x[i] = x[i];
            h->im_ref[i] = ref;
            h->im_hw[i] = hw[i];
        }
    }
    h->im_calls++;
}

void echo_synth_hook(const int32_t *work, const int16_t *out, int nch, int ch, uint32_t sw)
{
    if (!syn_ready)
        syn_reset();
    volatile __typeof__(ctl->hws) *h = &ctl->hws;
    int32_t sh = h->shift;
    uint32_t tsyn = SYN_NOW();
    for (int s = 0; s < 18; s++) {
        int32_t x[32];
        for (int sb = 0; sb < 32; sb++)
            x[sb] = sh >= 0 ? work[sb * 18 + s] >> sh : (int32_t)((uint32_t)work[sb * 18 + s] << -sh);
        uint32_t st0 = REG(SYN + 0x80c);
        REG(SYN + 0x804) = (uint32_t)ch | (15 - syn_phase[ch]) << 1;
        for (int sb = 0; sb < 32; sb++)
            REG(SYN + 4 * sb) = (uint32_t)x[sb];
        syn_phase[ch] = (syn_phase[ch] + 1) & 15;
        /* done: module 73 learns it by interrupt; here, +0x80c nonzero or 200 us */
        uint32_t t0 = SYN_NOW(), st = 0;
        int polled = 0;
        while (SYN_NOW() - t0 < 200 * 24u)
            if ((st = REG(SYN + 0x80c)) != 0) {
                polled = 1;
                break;
            }
        h->polled += polled;
        h->timeouts += !polled;
        REG(SYN + 0x80c) = 0x11;                /* acknowledge, as module 73 */
        REG(SYN + 0x810) = 0;
        int16_t hw[32];
        for (int k = 0; k < 32; k++)
            hw[k] = (int16_t)REG(SYN + 4 * k);
        for (int k = 0; k < 32; k++) {
            int32_t sw = out[(s * 32 + k) * nch];
            int32_t d = hw[k] - sw, r = hw[31 - k] - sw;
            uint32_t a = d < 0 ? -d : d, ra = r < 0 ? -r : r;
            h->exact += a == 0;
            h->within1 += a <= 1;
            h->within4 += a <= 4;
            h->r_exact += ra == 0;
            h->r_within4 += ra <= 4;
            if (a > h->max_err)
                h->max_err = a;
            h->sum_sw += sw < 0 ? -sw : sw;
            h->sum_hw += hw[k] < 0 ? -hw[k] : hw[k];
            h->dot += (int64_t)sw * hw[k];
        }
        if (s == 17) {
            h->syn_hw_ticks += SYN_NOW() - tsyn;    /* (the comparisons included: an upper bound) */
            h->syn_sw_ticks += sw;
            h->syn_timed++;
        }
        if (h->slots == 0) {
            for (int k = 0; k < 4; k++) {
                h->x[k] = x[k];
                h->sw[k] = out[k * nch];
                h->hw[k] = hw[k];
            }
            h->st_before = st0;
            h->st_after = st;
        }
        h->slots++;
    }
}
