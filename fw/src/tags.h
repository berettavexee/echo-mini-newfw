/* Tags for the play screen (ETUDE §25) */
#ifndef TAGS_H
#define TAGS_H

#include <stdint.h>
#include "fat.h"

struct tags {
    char title[96], artist[128], album[128];    /* UTF-8, empty when absent */
    uint16_t track;
    int16_t rg_gain[2];                         /* ReplayGain, track and album: 1/100 dB */
    uint32_t rg_peak[2];                        /* their peaks, Q16 (65536 = full scale), 0: none */
    uint32_t ogg_end_page, ogg_end_seg;         /* Ogg: end of the comment header (hifi_proto.h) */
};

#define RG_NONE  (-32768)                      /* rg_gain: no tag */
enum { RG_TRACK, RG_ALBUM };

/* Reads the tags of f, a file of our codec 'codec' (hifi_proto.h) */
void tags_read(const struct fat_file *f, uint32_t codec, struct tags *t);

#endif
