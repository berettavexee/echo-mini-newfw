/* ALAC decoder for core B (alacdec.c, derived from Apple's, Apache-2.0) */
#ifndef ALACDEC_H
#define ALACDEC_H

#include <stdint.h>

struct alac {
    uint32_t frame_len, bits, pb, mb, kb, channels, max_run, max_frame_bytes, rate;
    int32_t *pred, *u, *v;              /* frame_len words each */
};

/* The ALACSpecificConfig (24 bytes, possibly after 'frma' and 'alac' atom headers); the three
 * buffers from alloc. 0, or -1 when the configuration is not one we play. */
int alac_init(struct alac *a, const uint8_t *cookie, uint32_t n, void *(*alloc)(uint32_t));
/* One packet (followed by 8 readable bytes) into out, in the format of core B's ring: 16-bit
 * packed L | R << 16 (one word per frame), 20 and 24-bit one word per sample, data in the high
 * bits (two words per frame); mono duplicated. 0 and *frames, or -1 on a damaged packet. */
int alac_decode(struct alac *a, const uint8_t *pkt, uint32_t n, uint32_t *out, uint32_t *frames);

#endif
