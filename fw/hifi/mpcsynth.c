/* Musepack's synthesis on SYNTH (ETUDE §77) instead of libmpcdec's
 * mpc_decoder_synthese_filter_float, called from synth_filter.c: per frame and channel, 36 slots
 * of 32 subband samples (Y_L, Y_R), written >> 1: libmpcdec keeps them 32 times larger than
 * pvmp3dec, and SYNTH takes pvmp3dec's << 4 (mp3synth.c). Checked on the PC with pvmp3dec's
 * synthesis as the model of SYNTH: within 1 LSB of libmpcdec on 12 whole tracks, same signs,
 * same delay. The 16-bit PCM goes back into libmpcdec's output at its scale
 * (<< MPC_FIXED_POINT_FRACTPART), so codec_run is unchanged. The history is cleared at every
 * codec_open (each track, and a seek, which opens afresh). After a timeout the frame is done in
 * software, and after 3 the rest of the track. -DECHO_HWTEST: both computed, the sound from
 * libmpcdec, compared (hwp: line). Included by codec_mpc.c. */
#include "synth.h"

void echo_mpc_synth_reset(void)
{
    syn_clear();
#ifdef ECHO_HWTEST
    uint32_t trial = ctl->hws.trial;            /* MP3's bench (hwtest_synth.c) */
    memset((void *)(uintptr_t)&ctl->hws, 0, sizeof ctl->hws);
    ctl->hws.magic = HWT_MAGIC;
    ctl->hws.trial = trial;
#endif
}

/* The 36 slots of one channel (y: Y_L or Y_R) into out (stride nch): 1 done, 0 timeout */
static int mpc_synth_ch(const int32_t *y, int32_t *out, int nch, uint32_t ch)
{
    for (int s = 0; s < 36; s++, y += 32) {
        syn_begin(ch);
        for (int sb = 0; sb < 32; sb++)
            SYN_IO(sb) = (uint32_t)(y[sb] >> 1);
        if (!syn_wait())
            return 0;
        int32_t *o = out + s * 32 * nch;
        for (int k = 0; k < 32; k++)
            o[k * nch] = (int16_t)SYN_IO(k) * (1 << MPC_FIXED_POINT_FRACTPART);
    }
    return 1;
}

#ifndef ECHO_HWTEST
/* 1: the frame's synthesis done on SYNTH; 0: libmpcdec's for this frame */
int echo_mpc_synth(const int32_t *yl, const int32_t *yr, int32_t *out, int nch)
{
    if (syn_timeouts >= 3)
        return 0;
    for (int c = 0; c < nch; c++)
        if (!mpc_synth_ch(c ? yr : yl, out + c, nch, (uint32_t)c)) {
            ctl->synth_soft++;
            return 0;
        }
    ctl->synth_gran += 2 * (uint32_t)nch;      /* 36 slots: two MP3 granules' worth */
    return 1;
}

void echo_mpc_synth_check(const int32_t *out, int nch)
{
    (void)out;
    (void)nch;
}
#else
/* Test build: SYNTH's result kept aside and timed, libmpcdec computes the sound, then both are
 * compared on the 16-bit output codec_run makes of it */
static int32_t hw_out[2 * 36 * 32];
static int hw_ok;
static uint32_t t_sw;

int echo_mpc_synth(const int32_t *yl, const int32_t *yr, int32_t *out, int nch)
{
    (void)out;
    hw_ok = 0;
    if (syn_timeouts < 3) {
        uint32_t t0 = TIMER1_NOW();
        hw_ok = 1;
        for (int c = 0; c < nch && hw_ok; c++)
            hw_ok = mpc_synth_ch(c ? yr : yl, hw_out + c, nch, (uint32_t)c);
        ctl->hws.p_hw_ticks += TIMER1_NOW() - t0;
        if (!hw_ok)
            ctl->hws.p_timeouts++;
    }
    t_sw = TIMER1_NOW();
    return 0;
}

static int32_t pcm16(int32_t v)
{
    int32_t x = v >> MPC_FIXED_POINT_FRACTPART;
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

void echo_mpc_synth_check(const int32_t *out, int nch)
{
    ctl->hws.syn_sw_ticks += TIMER1_NOW() - t_sw;
    if (!hw_ok)
        return;
    uint32_t n = 36 * 32 * (uint32_t)nch;
    for (uint32_t i = 0; i < n; i++) {
        int32_t d = pcm16(hw_out[i]) - pcm16(out[i]);
        uint32_t a = d < 0 ? (uint32_t)-d : (uint32_t)d;
        ctl->hws.p_exact += a == 0;
        ctl->hws.p_within1 += a <= 1;
        if (a > ctl->hws.p_max)
            ctl->hws.p_max = a;
    }
    ctl->hws.p_cmp += n;
    ctl->hws.p_gran++;
}
#endif
