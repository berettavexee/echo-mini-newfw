/* MP4 audio (M4A) read by core A for the player (ETUDE §55) */
#ifndef MP4_H
#define MP4_H

#include <stdint.h>
#include "fat.h"

enum { MP4_OK, MP4_EFORMAT = -1, MP4_EREAD = -2, MP4_EALAC = -3, MP4_EUNSUPPORTED = -4 };

struct mp4_track {
    uint32_t rate, channels;
    uint32_t length;                    /* frames once the delay and padding are removed */
    uint32_t delay, padding;            /* encoder delay and padding, frames */
    uint32_t samples;                   /* AAC frames in the track */
};

/* Reads the container of f and prepares the packet stream from seek_ms (0: the start) */
int mp4_open(const struct fat_file *f, uint32_t seek_ms, struct mp4_track *t);
/* Writes up to 'room' bytes of the packet stream into the input ring at stream offset wr;
 * returns how many; *reads counts the file reads */
uint32_t mp4_feed(uint32_t wr, uint32_t room, uint32_t *reads);
int mp4_done(void);                     /* the whole stream written */

#endif
