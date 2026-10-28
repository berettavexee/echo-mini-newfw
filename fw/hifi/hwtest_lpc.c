/* Test build only (make EXTRA=-DECHO_HWTEST, ETUDE §66): every LPC subframe dr_flac decodes is
 * computed again on HIFIACC's FIR through the DMAC2, with the register map of ETUDE §1.5, and
 * compared with dr_flac's samples; counts per order, times (HIFIACC against dr_flac's own
 * prediction), the first difference and the first order-1 difference go to ctl->hwt, which core
 * A logs. The output stays dr_flac's. Included by codec_flac.c. */
#include "accel.h"

#define HWT_MAX_N   10240u                      /* larger blocks are counted as skipped */
#define HWT_SEG     8191u                       /* HIFIACC's length field: 13 bits */
#define HWT_CHUNK   2048u                       /* words per DMA block (LLI chain) */

struct lli { uint32_t sar, dar, llp, ctl_lo, ctl_hi, sstat, dstat; };
static int32_t hwt_x[HWT_MAX_N], hwt_y[HWT_MAX_N];
static struct lli lli_in[5], lli_out[5];
static int hwt_ready, hwt_timeouts;             /* timeouts: the test stops after 3 in a track */

static void dma_chain(struct lli *l, uint32_t src, uint32_t dst, int src_inc, uint32_t n,
                      uint32_t ctl_chain, uint32_t ctl_last)
{
    uint32_t k = 0;
    for (int i = 0; k < n; i++) {
        uint32_t m = n - k < HWT_CHUNK ? n - k : HWT_CHUNK;
        l[i].sar = src_inc ? src + 4 * k : src;
        l[i].dar = src_inc ? dst : dst + 4 * k;
        l[i].ctl_hi = m;
        k += m;
        l[i].llp = k < n ? (uint32_t)&l[i + 1] : 0;
        l[i].ctl_lo = k < n ? ctl_chain : ctl_last;
        l[i].sstat = l[i].dstat = 0;
    }
}

static void dma_channel(int ch, const struct lli *l, uint32_t cfg, uint32_t cfg_h)
{
    /* the first block in the registers too: without a chain the controller does not read the
     * LLI (HW1: the channel kept the last addresses); a chain starts from its first LLI (HW2) */
    REG(DMA_CH(ch) + 0x00) = l[0].sar;
    REG(DMA_CH(ch) + 0x08) = l[0].dar;
    REG(DMA_CH(ch) + 0x10) = l[0].llp ? (uint32_t)l : 0;
    REG(DMA_CH(ch) + 0x18) = l[0].ctl_lo;
    REG(DMA_CH(ch) + 0x1c) = l[0].ctl_hi;
    REG(DMA_CH(ch) + 0x40) = cfg;
    REG(DMA_CH(ch) + 0x44) = cfg_h;
}

/* HIFIACC on in[0..n) into out[0..n), n <= HWT_SEG: 1 done, 0 timeout */
static int hwt_run(const int32_t *in, int32_t *out, uint32_t n, uint32_t order, uint32_t shift)
{
    if (!hwt_ready) {
        REG(DMA + 0x398) = 1;                   /* DmaCfgReg: on */
        REG(HACC) = 0x01080108;                 /* as RECHO and the stock module at start */
        hwt_ready = 1;
    }
    REG(DMA + 0x338) = 3;                       /* ClearTfr, ClearBlock, ClearErr */
    REG(DMA + 0x340) = 3;
    REG(DMA + 0x358) = 3;
    REG(HACC + 0x04) |= 0x8000;                 /* LPC mode */
    REG(HACC + 0x2c) = order << 16 | shift << 8;
    REG(HACC + 0x0c) = 0xa0000000u | n << 15;
    /* channel 1: input -> input FIFO; channel 0: output FIFO -> output */
    dma_chain(lli_in, (uint32_t)in, HACC_FIN, 1, n, 0x1a109125, 0x02109125);
    dma_chain(lli_out, HACC_FOUT, (uint32_t)out, 0, n, 0x18a09425, 0x00a09425);
    dma_channel(1, lli_in, 0x800, 0x807);
    dma_channel(0, lli_out, 0x400, 7);
    REG(HACC + 0x0c) = 0xa0000000u | n << 15 | 3;   /* start */
    REG(DMA + 0x3a0) = 0x202;                   /* ChEnReg: channel 1, then 0 */
    REG(DMA + 0x3a0) = 0x101;
    uint32_t t0 = TIMER1_NOW();
    int ok = 0;
    while (TIMER1_NOW() - t0 < 5 * 24000u)       /* 5 ms (a 4608 block takes about 0.2 ms) */
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

void echo_lpc_hook(const int *y, unsigned n, unsigned bps, unsigned order, int shift,
                   unsigned prec, const int *qlp, unsigned sw_ticks)
{
    volatile __typeof__(ctl->hwt) *h = &ctl->hwt;
    if (h->magic != HWT_MAGIC) {
        memset((void *)(uintptr_t)h, 0, sizeof *h);
        h->magic = HWT_MAGIC;
        hwt_timeouts = 0;
    }
    h->sub++;
    if (hwt_timeouts >= 3 || n > HWT_MAX_N || n <= order || order > 32) {
        h->skipped++;
        return;
    }
    int bits = 0;
    for (uint32_t o = order; o; o >>= 1)
        bits++;
    int wide = bps + prec + bits > 32;
    /* the input HIFIACC takes: the warm-up samples, then the exact residuals */
    for (uint32_t i = 0; i < order; i++)
        hwt_x[i] = y[i];
    for (uint32_t i = order; i < n; i++) {
        int64_t acc = 0;
        for (uint32_t j = 0; j < order; j++)
            acc += (int64_t)qlp[j] * y[i - 1 - j];
        hwt_x[i] = y[i] - (int32_t)(acc >> shift);
    }
    uint32_t t2 = TIMER1_NOW();
    for (uint32_t k = 0; k < order; k++)        /* reverse order: COEF[k] weighs y[n-order+k] */
        REG(HACC_COEF + 4 * k) = (uint32_t)qlp[order - 1 - k];
    for (uint32_t k = order; k < 32; k++)       /* HW5: the unused taps to 0 (order 1 read a */
        REG(HACC_COEF + 4 * k) = 0;             /* stale second one, HW4) */
    /* blocks over HWT_SEG in passes: each later pass gets the last `order` outputs of the one
     * before as its warm-up (written over the residuals already used), so the result is exact */
    int ok = 1;
    uint32_t s = 0, segs = 0;
    while (ok && s < n) {
        uint32_t from = s ? s - order : 0, len = n - from < HWT_SEG ? n - from : HWT_SEG;
        if (s)
            for (uint32_t i = 0; i < order; i++)
                hwt_x[from + i] = hwt_y[from + i];
        ok = hwt_run(hwt_x + from, hwt_y + from, len, order, (uint32_t)shift);
        if (len > h->max_seg)
            h->max_seg = len;
        s = from + len;
        segs++;
    }
    uint32_t t3 = TIMER1_NOW();
    if (segs > 1)
        h->split++;
    if (order > h->max_order)
        h->max_order = order;
    if ((uint32_t)shift > h->max_shift)
        h->max_shift = shift;
    if (n > h->max_n)
        h->max_n = n;
    h->wide += wide;
    if (!ok) {
        h->timeout++;
        hwt_timeouts++;
        return;
    }
    if (sw_ticks) {                             /* dr_flac timed one subframe in four (HW5) */
        h->sw_ticks += sw_ticks;
        h->hw_ticks += t3 - t2;
    }
    uint32_t i = 0;
    while (i < n && hwt_y[i] == y[i])
        i++;
    if (i == n) {
        h->exact++;
        h->ok_o[order - 1]++;
        return;
    }
    h->diff++;
    h->diff_o[order - 1]++;
    h->wide_diff += wide;
    h->diff_orders |= 1u << ((order - 1) & 31);
    if (h->diff == 1) {
        h->f_order = order;
        h->f_shift = shift;
        h->f_prec = prec;
        h->f_bps = bps;
        h->f_n = n;
        h->f_idx = i;
        h->f_sw = y[i];
        h->f_hw = hwt_y[i];
    }
    if (order == 1 && h->o1_coef == 0 && h->o1_shift == 0) {
        for (uint32_t k = 0; k < 6 && k < n; k++) {
            h->o1_x[k] = hwt_x[k];
            h->o1_sw[k] = y[k];
            h->o1_hw[k] = hwt_y[k];
        }
        h->o1_coef = qlp[0];
        h->o1_shift = shift ? shift : -1;       /* -1: shift 0 (keeps "not set" apart) */
    }
}
