/* Test build only (make EXTRA=-DECHO_HWTEST, ETUDE §113): each ALAC prediction of order 1 to 30
 * our decoder computes (alacdec.c) is computed again on HIFIACC's ALAC filter, with the sequence
 * of the stock module 79, and compared; the sound stays our decoder's. The input is the first
 * order + 1 samples our decoder output (the warm-up) then the residuals; the output is in place.
 * Each prediction runs twice: the coefficients reversed (FFmpeg's order, the stock module's: all
 * the counts, per order and lpc_quant, the first difference, times and timeouts), then in stream
 * order (ours: exact or not only, to show that the bench sees a wrong order). In ctl->hwa,
 * logged by core A at the end of the track. Included by codec_alac.c. */
#include "accel.h"

#define HWA_MAX_N   8191u                       /* the length field: 13 bits */
#define HWA_CHUNK   2048u                       /* words per DMA block (LLI chain) */

struct lli { uint32_t sar, dar, llp, ctl_lo, ctl_hi, sstat, dstat; };
static int32_t hwa_buf[HWA_MAX_N];
static struct lli hwa_in[4], hwa_out[4];
static int16_t hwa_coefs[32];
static uint32_t hwa_t0;
static int hwa_ready;

static void hwa_chain(struct lli *l, uint32_t src, uint32_t dst, int src_inc, uint32_t n,
                      uint32_t ctl_chain, uint32_t ctl_last)
{
    uint32_t k = 0;
    for (int i = 0; k < n; i++) {
        uint32_t m = n - k < HWA_CHUNK ? n - k : HWA_CHUNK;
        l[i].sar = src_inc ? src + 4 * k : src;
        l[i].dar = src_inc ? dst : dst + 4 * k;
        l[i].ctl_hi = m;
        k += m;
        l[i].llp = k < n ? (uint32_t)&l[i + 1] : 0;
        l[i].ctl_lo = k < n ? ctl_chain : ctl_last;
        l[i].sstat = l[i].dstat = 0;
    }
}

static void hwa_channel(int ch, const struct lli *l, uint32_t cfg, uint32_t cfg_h)
{
    /* the first block in the registers too (ETUDE §66, HW1 and HW2) */
    REG(DMA_CH(ch) + 0x00) = l[0].sar;
    REG(DMA_CH(ch) + 0x08) = l[0].dar;
    REG(DMA_CH(ch) + 0x10) = l[0].llp ? (uint32_t)l : 0;
    REG(DMA_CH(ch) + 0x18) = l[0].ctl_lo;
    REG(DMA_CH(ch) + 0x1c) = l[0].ctl_hi;
    REG(DMA_CH(ch) + 0x40) = cfg;
    REG(DMA_CH(ch) + 0x44) = cfg_h;
}

/* Before our predictor: 1 when this one is tested (its coefficients kept: ours adapts them) */
int echo_alac_pre(const int16_t *coefs, uint32_t act, uint32_t num)
{
    volatile __typeof__(ctl->hwa) *h = &ctl->hwa;
    if (h->magic != HWT_MAGIC) {                /* the control block is cleared at each track */
        memset((void *)(uintptr_t)h, 0, sizeof *h);
        h->magic = HWT_MAGIC;
    }
    if (act < 1 || act > 30 || num <= act + 1)
        return 0;                               /* a copy, or the first difference (31) */
    if (num > HWA_MAX_N || h->timeouts >= 3) {
        h->skipped++;
        return 0;
    }
    for (uint32_t i = 0; i < act; i++)
        hwa_coefs[i] = coefs[i];
    hwa_t0 = TIMER1_NOW();
    return 1;
}

/* One run of the filter on hwa_buf (warm-up, residuals; in place): 1 done, 0 timeout */
static int hwa_run(uint32_t num, uint32_t act, uint32_t chanbits, uint32_t denshift, int reversed)
{
    if (!hwa_ready) {
        REG(DMA + 0x398) = 1;                   /* DmaCfgReg: on */
        REG(HACC) = 0x01080108;                 /* as the stock modules at start */
        hwa_ready = 1;
    }
    uint32_t t0 = TIMER1_NOW();
    REG(DMA + 0x338) = 3;                       /* ClearTfr, ClearBlock, ClearErr */
    REG(DMA + 0x340) = 3;
    REG(DMA + 0x358) = 3;
    /* the stock module 79's order: the function and length, the mode, the configuration, the
     * coefficients, the start */
    REG(HACC + 0x0c) = 0x60000000u | num << 15;
    REG(HACC + 0x04) |= 0x8000;
    REG(HACC + 0x28) = act << 16 | denshift << 8 | chanbits << 24;
    for (uint32_t k = 0; k < act; k++)
        REG(HACC_ACOEF + 4 * k) = (uint32_t)(int32_t)(reversed ? hwa_coefs[act - 1 - k]
                                                              : hwa_coefs[k]);
    hwa_chain(hwa_in, (uint32_t)hwa_buf, HACC_FIN, 1, num, 0x1a109125, 0x02109125);
    hwa_chain(hwa_out, HACC_FOUT, (uint32_t)hwa_buf, 0, num, 0x18a09425, 0x00a09425);
    hwa_channel(1, hwa_in, 0x800, 0x807);
    hwa_channel(0, hwa_out, 0x400, 7);
    REG(HACC + 0x0c) = 0x60000000u | num << 15 | 3;     /* start */
    REG(DMA + 0x3a0) = 0x202;                   /* ChEnReg: channel 1, then 0 */
    REG(DMA + 0x3a0) = 0x101;
    int ok = 0;
    while (TIMER1_NOW() - t0 < 30 * 24000u)     /* 30 ms, as hifiacc.c */
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

/* The input of a run: our warm-up, then the residuals */
static void hwa_fill(const int32_t *pc, const int32_t *out, uint32_t num, uint32_t act)
{
    for (uint32_t i = 0; i < num; i++)
        hwa_buf[i] = i <= act ? out[i] : pc[i];
}

/* The first sample after the warm-up where hwa_buf differs from out, or num */
static uint32_t hwa_cmp(const int32_t *out, uint32_t num, uint32_t act)
{
    for (uint32_t i = act + 1; i < num; i++)
        if (hwa_buf[i] != out[i])
            return i;
    return num;
}

/* After it: the same on the filter, compared with out */
void echo_alac_post(const int32_t *pc, const int32_t *out, uint32_t num, uint32_t act,
                    uint32_t chanbits, uint32_t denshift)
{
    volatile __typeof__(ctl->hwa) *h = &ctl->hwa;
    uint32_t sw = TIMER1_NOW() - hwa_t0;
    hwa_fill(pc, out, num, act);
    uint32_t t0 = TIMER1_NOW();
    int ok = hwa_run(num, act, chanbits, denshift, 1);
    h->hw_ticks += TIMER1_NOW() - t0;
    h->sw_ticks += sw;
    h->subframes++;
    if (!ok) {
        h->timeouts++;
        return;
    }
    uint32_t bad = hwa_cmp(out, num, act), quant = denshift & 15;
    if (bad == num) {
        h->exact++;
        h->ok_o[act]++;
        h->ok_q[quant]++;
    } else {
        if (!h->differ) {
            h->f_order = act;
            h->f_quant = denshift;
            h->f_bps = chanbits;
            h->f_n = num;
            h->f_idx = bad;
            h->f_sw = out[bad];
            h->f_hw = hwa_buf[bad];
        }
        h->differ++;
        h->diff_o[act]++;
        h->diff_q[quant]++;
    }
    hwa_fill(pc, out, num, act);                /* the other order, for comparison */
    if (hwa_run(num, act, chanbits, denshift, 0)) {
        if (hwa_cmp(out, num, act) == num)
            h->s_exact++;
        else
            h->s_differ++;
    } else
        h->timeouts++;
}
