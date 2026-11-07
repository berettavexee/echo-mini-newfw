/* DMAC2 for HIFIACC's FIFOs (ETUDE §66): a block of n words in at most a few linked lists of
 * HACC_CHUNK words, the input from memory into +0x40, the output from +0xa0 into memory. Shared
 * by hifiacc.c (FLAC), alacacc.c (ALAC) and hwtest_alac.c. */
#ifndef HACCDMA_H
#define HACCDMA_H

#include "accel.h"

#define HACC_CHUNK  2048u                       /* words per DMA block (LLI chain) */

struct lli { uint32_t sar, dar, llp, ctl_lo, ctl_hi, sstat, dstat; };

/* l: enough entries for n / HACC_CHUNK rounded up; src_inc: memory to the FIFO (else the FIFO
 * to memory) */
static inline void hacc_dma_chain(struct lli *l, uint32_t src, uint32_t dst, int src_inc,
                                  uint32_t n, uint32_t ctl_chain, uint32_t ctl_last)
{
    uint32_t k = 0;
    for (int i = 0; k < n; i++) {
        uint32_t m = n - k < HACC_CHUNK ? n - k : HACC_CHUNK;
        l[i].sar = src_inc ? src + 4 * k : src;
        l[i].dar = src_inc ? dst : dst + 4 * k;
        l[i].ctl_hi = m;
        k += m;
        l[i].llp = k < n ? (uint32_t)&l[i + 1] : 0;
        l[i].ctl_lo = k < n ? ctl_chain : ctl_last;
        l[i].sstat = l[i].dstat = 0;
    }
}

static inline void hacc_dma_channel(int ch, const struct lli *l, uint32_t cfg, uint32_t cfg_h)
{
    /* the first block in the registers too: without a chain the controller does not read the
     * LLI; a chain starts from its first LLI (ETUDE §66, HW1 and HW2) */
    REG(DMA_CH(ch) + 0x00) = l[0].sar;
    REG(DMA_CH(ch) + 0x08) = l[0].dar;
    REG(DMA_CH(ch) + 0x10) = l[0].llp ? (uint32_t)l : 0;
    REG(DMA_CH(ch) + 0x18) = l[0].ctl_lo;
    REG(DMA_CH(ch) + 0x1c) = l[0].ctl_hi;
    REG(DMA_CH(ch) + 0x40) = cfg;
    REG(DMA_CH(ch) + 0x44) = cfg_h;
}

/* The two channels of a block in place or not: in (n words) to the FIFO, the FIFO to out */
static inline void hacc_dma_block(struct lli *li, struct lli *lo, const int32_t *in, int32_t *out,
                                  uint32_t n)
{
    hacc_dma_chain(li, (uint32_t)in, HACC_FIN, 1, n, 0x1a109125, 0x02109125);
    hacc_dma_chain(lo, HACC_FOUT, (uint32_t)out, 0, n, 0x18a09425, 0x00a09425);
    hacc_dma_channel(1, li, 0x800, 0x807);
    hacc_dma_channel(0, lo, 0x400, 7);
}

#endif
