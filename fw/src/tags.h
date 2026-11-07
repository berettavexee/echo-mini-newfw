/* Tags for the play screen (ETUDE §25) */
#ifndef TAGS_H
#define TAGS_H

#include <stdint.h>
#include "fat.h"

struct tags {
    char title[96], artist[128], album[128];    /* UTF-8, empty when absent */
    uint16_t track;
    char albumartist[96], genre[48];            /* (the library, ETUDE §122) */
    uint16_t year, disc;                        /* 0 when absent */
    int16_t rg_gain[2];                         /* ReplayGain, track and album: 1/100 dB */
    uint32_t rg_peak[2];                        /* their peaks, Q16 (65536 = full scale), 0: none */
    uint32_t ogg_end_page, ogg_end_seg;         /* Ogg: end of the comment header (hifi_proto.h) */
};

#define RG_NONE  (-32768)                      /* rg_gain: no tag */
enum { RG_TRACK, RG_ALBUM };

/* Reads the tags of f, a file of our codec 'codec' (hifi_proto.h); not reentrant */
void tags_read(const struct fat_file *f, uint32_t codec, struct tags *t);
extern void (*tags_yield)(void);
/* set (the library's build): only the fields; an Ogg's comment header is not crossed to its end
 * (ogg_end_page, core B's hint, left 0), a cover that is the last comment not crossed at all */
extern int tags_fields_only;    /* called before each read when set (the storage task) */

#endif
