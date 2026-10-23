/* Ogg (RFC 3533) packets from the input ring, for the Opus and Vorbis codecs of core B */
#ifndef OGGPAGE_H
#define OGGPAGE_H

#include <stdint.h>

struct pg_state {
    uint32_t serial;            /* the logical stream followed (the first one met) */
    int have_serial;
    uint8_t lace[255];
    int nsegs, seg;
    uint8_t flags;              /* of the current page: 1 continued, 2 first, 4 last */
    int64_t granule;            /* of the current page */
    uint32_t resyncs, skipped;  /* pages lost to a bad header, packets too long for the buffer */
};

extern struct pg_state pg;

void pg_reset(void);
/* Next packet into buf (at most max bytes): its length, -1 for a packet longer than max (its
 * first max bytes are in buf, the rest was skipped), 0 at the end of the stream. pg_last_on_page() then tells whether it was the
 * last packet completed on its page, whose granule (pg.granule) is the position after it. */
int pg_packet(uint8_t *buf, uint32_t max);
int pg_last_on_page(void);
/* Goes on from segment 'seg' of the page at file offset 'page' (forward only): 0, or -1 */
int pg_skip_to(uint32_t page, uint32_t seg);
/* Goes on from the next page found at or after file offset 'off' (forward only): 0, or -1 */
int pg_jump(uint32_t off);

#endif
