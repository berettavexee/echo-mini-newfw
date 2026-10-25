/* Core B runtime for our codec images: rings, control block, sleep (see src/hifi_proto.h) */
#ifndef RT_H
#define RT_H

#include <stdint.h>
#include "../src/hifi_proto.h"

#define ctl HP_CTL

/* Implemented by each codec. Both may also block in rt_wait() (input or output). */
extern const uint32_t codec_id;
int codec_open(void);           /* 0: format known (ctl rate, channels, bits, frame_bytes,
                                   total_frames set), 1: needs more input, < 0: enum hp_err */
int codec_run(void);            /* 1: did some work, 0: blocked (input or output), 2: end of
                                   the stream, < 0: error */

void rt_wait(void);             /* sleeps until core A rings (new input, room, command) */
/* Heap for the codec, from the end of BSS to HP_DATA_END; rt_heap_reset() frees everything */
void *rt_alloc(uint32_t n);
void rt_heap_reset(void);
extern uint32_t rt_heap_peak;

/* Input ring */
uint32_t in_avail(void);        /* bytes written by core A and not consumed */
int in_ended(void);             /* core A has written the whole stream */
uint32_t in_peek(void *dst, uint32_t n);    /* copies up to n bytes without consuming */
/* Bytes [off, off+n) ahead of the read position, without consuming: waits until core A has
 * written them, the input ended or the ring is as full as core A keeps it (at least 24 KiB);
 * returns how many were copied */
uint32_t in_peek_ahead(uint32_t off, void *dst, uint32_t n);
uint32_t in_read(void *dst, uint32_t n);    /* copies and consumes up to n bytes */
void in_skip(uint32_t n);
uint32_t in_read_wait(void *dst, uint32_t n);   /* waits for n bytes (fewer at the end) */
int in_seek(uint32_t pos);      /* forward to stream offset pos: 0 ok; core A jumps past what
                                   it has not read yet */
/* Output ring: whole frames, 4-byte words */
uint32_t out_space(void);       /* bytes that can be written now */
void out_word(uint32_t w);      /* one word, the caller checked the space */
void out_write(const uint32_t *w, uint32_t words);   /* several, the caller checked the space */
void out_done(uint32_t frames);  /* frames written by the codec: ctl->frames (upsampled: the
                                   runtime counts the frames it writes instead) */

#endif
