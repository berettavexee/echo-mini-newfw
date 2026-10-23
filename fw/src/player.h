/* Step 5c player: one file through a stock codec module on core B, PCM into the ring */
#ifndef PLAYER_H
#define PLAYER_H

#include <stdint.h>
#include "fat.h"

struct player_state {
    int state;                  /* see enum in player.c */
    int error;
    uint32_t rate, channels, bps, length_ms, time_ms;
    uint32_t buffers, frames, reads, read_bytes, seeks;
    uint32_t wait_us_max;       /* longest time a decoded buffer waited for room in the ring */
    /* our codecs: core B in the last 250 ms window (step 5g) */
    uint32_t b_load;            /* per mille of the time awake */
    uint32_t b_mhz10;           /* tenths of MHz used */
    uint32_t b_out;             /* bytes in core B's output ring */
};

extern struct player_state player;

/* What is heard (our codecs): the track opened last, or, after a chained switch, the end of the
 * one before until the DMA is past its last frame (the DAC ring holds up to 1.1 s of it). 'gen'
 * goes up when the track opened last becomes the one heard. For the screen. */
struct player_heard {
    uint32_t rate, bps, length_ms, time_ms, gen;
};
extern struct player_heard player_heard;

/* Plays 'file' with overlay 'module' (76: WAV) of a stock image found in the root, 'codec'
 * being the stock codec number of the descriptor (3: WAV). 'ui' is called every 20 ms or so
 * (keys, volume, screen) and returns non-zero to stop. Returns 0 at the end of the file. */
int player_play(const struct fat_file *file, int module, int codec, int (*ui)(void));
/* The same with our own codec image 'codec' (hifi_proto.h) and our protocol. PLAY_CHAIN: keep
 * the DACs running into the next call (same rate and frame size), return at the end of the
 * track without draining; player_chain_end() drains and lets the next start stop them. */
#define PLAY_CHAIN 1
int player_play_own(const struct fat_file *file, uint32_t codec, int (*ui)(void), int flags);
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
void player_close(int stopped);
int player_is_open(void);
extern int player_clock;        /* core B clock: 0 automatic, else a fixed divider of the PLL */

#endif

