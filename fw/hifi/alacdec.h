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

/* The predictor of order act (1 to 30) on HIFIACC (alacacc.c, included by codec_alac.c): 1 done,
 * 0 when it stays in software (pc then untouched) */
int echo_alac_hw(const int32_t *pc, int32_t *out, uint32_t num, const int16_t *coefs,
                 uint32_t act, uint32_t chanbits, uint32_t denshift);

#ifdef ECHO_HWTEST
/* Test build only (ETUDE §113): hwtest_alac.c, around alacdec.c's predictor */
int echo_alac_pre(const int16_t *coefs, uint32_t act, uint32_t num);
void echo_alac_post(const int32_t *pc, const int32_t *out, uint32_t num, uint32_t act,
                    uint32_t chanbits, uint32_t denshift);
#endif

#endif
