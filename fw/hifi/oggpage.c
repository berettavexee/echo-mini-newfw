/* Ogg (RFC 3533) packets from the input ring. Pages of other logical streams are skipped; a
 * damaged header makes the reader look for the next "OggS"; with no page in the first 64 KiB
 * the input is not Ogg. Page bodies and packets that do
 * not fit the caller's buffer are skipped with in_seek(), one jump per page, so core A jumps over
 * cover art instead of reading it. */
#include <string.h>
#include "rt.h"
#include "oggpage.h"

struct pg_state pg;

#define PG_FIRST_MAX 65536                      /* bytes searched for the first page */

void pg_reset(void)
{
    memset(&pg, 0, sizeof pg);
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Next page header of our stream: 1, or 0 at the end of the input */
static int next_page(void)
{
    uint8_t h[27];
    uint32_t lost = 0;
    for (;;) {
        if (in_read_wait(h, 4) < 4)
            return 0;
        while (memcmp(h, "OggS", 4)) {          /* lost: slide one byte at a time */
            if (!pg.have_serial && ++lost > PG_FIRST_MAX)
                return 0;                       /* no page at the start: not an Ogg file */
            memmove(h, h + 1, 3);
            if (in_read_wait(h + 3, 1) < 1)
                return 0;
            if (!memcmp(h, "OggS", 4))
                pg.resyncs++;
        }
        if (in_read_wait(h + 4, 23) < 23 || in_read_wait(pg.lace, h[26]) < h[26])
            return 0;
        if (h[4] != 0) {                        /* version: not a page after all */
            pg.resyncs++;
            continue;
        }
        uint32_t serial = le32(h + 14), body = 0;
        for (int i = 0; i < h[26]; i++)
            body += pg.lace[i];
        if (!pg.have_serial) {
            pg.serial = serial;
            pg.have_serial = 1;
        } else if (serial != pg.serial) {
            if (in_seek(ctl->in_rd + body))
                return 0;
            continue;
        }
        pg.flags = h[5];
        pg.granule = (int64_t)((uint64_t)le32(h + 6) | (uint64_t)le32(h + 10) << 32);
        pg.nsegs = h[26];
        pg.seg = 0;
        return 1;
    }
}

int pg_packet(uint8_t *buf, uint32_t max)
{
    uint32_t len = 0, skip = 0;                 /* skip: bytes of this page not read yet */
    int started = 0;
    for (;;) {
        if (pg.seg == pg.nsegs) {
            if ((skip && in_seek(ctl->in_rd + skip)) || !next_page())
                return 0;
            skip = 0;
            if (!started && (pg.flags & 1)) {  /* the end of a packet we never saw */
                while (pg.seg < pg.nsegs) {
                    uint8_t l = pg.lace[pg.seg++];
                    skip += l;
                    if (l < 255)
                        break;
                }
                continue;
            }
        }
        uint8_t l = pg.lace[pg.seg++];
        uint32_t k = len < max ? max - len : 0; /* bytes of this segment kept */
        started = 1;
        if (k > l)
            k = l;
        if (k) {
            if ((skip && in_seek(ctl->in_rd + skip)) || in_read_wait(buf + len, k) < k)
                return 0;
            skip = 0;
        }
        skip += l - k;
        len += l;
        if (l < 255) {
            if (skip && in_seek(ctl->in_rd + skip))
                return 0;
            if (len > max) {
                pg.skipped++;
                return -1;
            }
            return (int)len;
        }
    }
}

int pg_jump(uint32_t off)
{
    if (off > ctl->in_rd && in_seek(off))
        return -1;
    pg.nsegs = pg.seg = 0;                      /* next_page() looks for "OggS" */
    return 0;
}

int pg_skip_to(uint32_t page, uint32_t seg)
{
    if (in_seek(page) || !next_page() || seg > (uint32_t)pg.nsegs)
        return -1;
    uint32_t skip = 0;
    while (pg.seg < (int)seg)
        skip += pg.lace[pg.seg++];
    return skip && in_seek(ctl->in_rd + skip) ? -1 : 0;
}

int pg_last_on_page(void)
{
    for (int i = pg.seg; i < pg.nsegs; i++)
        if (pg.lace[i] < 255)
            return 0;                           /* another packet ends on this page */
    return 1;
}
