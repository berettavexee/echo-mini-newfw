/* Player: our codecs on core B (hifi_proto.h), PCM into the DAC ring (ETUDE §19 onwards) */
#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include "fat.h"

struct player_state {
    int state;                  /* see enum in player.c */
    int error;
    uint32_t rate, channels, bps, length_ms, time_ms;
    uint32_t frames, reads, read_bytes;
    uint32_t codec;             /* the core B codec of the track (M4A: AAC or ALAC, from its content) */
    /* our codecs: core B in the last 250 ms window (step 5g) */
    uint32_t b_load;            /* per mille of the time awake */
    uint32_t b_mhz10;           /* tenths of MHz used */
};

extern struct player_state player;

/* What is heard (our codecs): the track opened last, or, after a chained switch, the end of the
 * one before until the DMA is past its last frame (the DAC ring holds up to 1.1 s of it). 'gen'
 * goes up when the track opened last becomes the one heard. For the screen. */
struct player_heard {
    uint32_t rate, bps, length_ms, time_ms, gen;
};
extern struct player_heard player_heard;

/* player_open() flag: keep the DACs running into the next track (same rate and frame size); the
 * track ends without a drain, player_chain_end() drains and lets the next start stop them */
#define PLAY_CHAIN 1
void player_chain_end(void);
/* The same without blocking, from the main loop (the end of the queue): 1 once the DMA is past
 * the last frame, 0 meanwhile */
int player_drain_service(void);
/* What is queued for the DACs is dropped (a skip, a stop): no end of a track to follow */
void player_drop_tail(void);
/* Non-blocking form: open (load, open, start the output: up to ~0.4 s), then service from the
 * main loop (0 while the track plays, 1 at its end, < 0 on an error), then close */
int player_open(const struct fat_file *file, uint32_t codec, int flags);
/* For the next player_open() only, Ogg: where the comment header ends (tags.h), 0: unknown */
void player_ogg_hint(uint32_t page, uint32_t seg);
/* For the next player_open() only: start at ms (0: at the start) */
void player_seek_hint(uint32_t ms);
int player_service(void);
/* player_service() error: input read on and on without a frame decoded (zeros, garbage) */
#define PLAYER_ENOAUDIO (-50)
void player_close(int stopped);
int player_is_open(void);
extern int player_clock;        /* core B clock: 0 automatic, else a fixed divider of the PLL */

#endif

