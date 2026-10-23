/* Glyphs of Fusion Pixel 12px from the font blob of our firmware image (tools/mkfusion.py).
 * The run table (first code point and first glyph of each run of consecutive codes) and the
 * offset of every 16th record stay in RAM (about 34 KiB); a missing glyph costs one eMMC
 * command: its group of 16 records (under 800 bytes) is read and all of them go into a
 * direct-mapped cache of 256 records. */
#include <stdint.h>
#include "emmc.h"
#include "font.h"
#include "log.h"

#define RANGES_MAX 4400
#define GROUPS_MAX 2600
#define GROUP      16
#define REC_MAX    52                           /* largest record: 49 bytes */
#define CACHE      256

static uint32_t first[RANGES_MAX];
static uint16_t index_[RANGES_MAX + 1];
static uint32_t group[GROUPS_MAX];
static uint32_t nranges, nglyphs, ngroups, recs_lba, recs_skip, recs_len;
static int ready;

static struct { uint32_t tag; uint8_t rec[REC_MAX]; } cache[CACHE];   /* tag = glyph index + 1 */
static uint32_t buf[4 * 512 / 4];
static uint32_t misses;

static int rec_len(const uint8_t *r)
{
    return 5 + r[2] * ((r[1] + 7) / 8);
}

/* bytes [off, off+n) of the blob at sector 'base' into dst, 4 sectors per eMMC command */
static int blob_read(uint32_t base, uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t s = off % 512, nsec = (s + n + 511) / 512, k;
        if (nsec > 4)
            nsec = 4;
        k = nsec * 512 - s;
        if (k > n)
            k = n;
        if (emmc_read(base + off / 512, nsec, buf))
            return -1;
        for (uint32_t i = 0; i < k; i++)
            d[i] = ((const uint8_t *)buf)[s + i];
        d += k;
        off += k;
        n -= k;
    }
    return 0;
}

int font_init(void)
{
    static uint32_t h[128];
    for (int k = 0; k < 2; k++) {
        uint32_t img = EMMC_FW1 + (k ? emmc.r : 0);     /* FW1, then FW2 */
        const uint8_t *hb = (const uint8_t *)h;
        if (emmc_read(img, 1, h) || hb[0x1f8] != 'R' || hb[0x1ff] != 'W')
            continue;
        uint32_t off = h[0x80 / 4], len = h[0x84 / 4];
        if (!len || off % 512 || emmc_read(img + off / 512, 1, h) || h[0] != 0x46585046 ||
            h[1] != 1)                          /* "FPXF", version 1 */
            continue;
        uint32_t base = img + off / 512;
        nglyphs = h[2];
        nranges = h[3];
        ngroups = h[10];
        uint32_t o_first = h[5], o_index = h[6], o_group = h[7], o_recs = h[8];
        recs_len = h[9];
        if (nranges > RANGES_MAX || ngroups > GROUPS_MAX || o_recs + recs_len > len ||
            blob_read(base, o_first, first, 4 * nranges) ||
            blob_read(base, o_index, index_, 2 * (nranges + 1)) ||
            blob_read(base, o_group, group, 4 * ngroups))
            continue;
        recs_lba = base + o_recs / 512;
        recs_skip = o_recs % 512;
        ready = 1;
        log_printf("font: Fusion Pixel 12px from %s, %u glyphs, %u runs", k ? "FW2" : "FW1",
                   nglyphs, nranges);
        return 0;
    }
    log_printf("font: no font blob in FW1 or FW2: ASCII only");
    return -1;
}

static int find(uint32_t c)                     /* glyph index of c, or -1 */
{
    uint32_t lo = 0, hi = nranges;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (first[mid] <= c)
            lo = mid;
        else
            hi = mid;
    }
    if (!nranges || c < first[lo] || c - first[lo] >= (uint32_t)(index_[lo + 1] - index_[lo]))
        return -1;
    return index_[lo] + (int)(c - first[lo]);
}

/* reads the group of glyph i into the cache */
static int fill(uint32_t i)
{
    uint32_t g = i / GROUP, start = recs_skip + group[g];
    uint32_t end = g + 1 < ngroups ? recs_skip + group[g + 1] : recs_skip + recs_len;
    uint32_t sec0 = start / 512, nsec = (end + 511) / 512 - sec0;
    if (nsec > 3 || emmc_read(recs_lba + sec0, nsec, buf))
        return -1;
    misses++;
    const uint8_t *p = (const uint8_t *)buf + start % 512;
    for (uint32_t j = g * GROUP; j < nglyphs && j < (g + 1) * GROUP; j++) {
        int n = rec_len(p);
        if (n > REC_MAX)
            return -1;
        cache[j % CACHE].tag = j + 1;
        for (int b = 0; b < n; b++)
            cache[j % CACHE].rec[b] = p[b];
        p += n;
    }
    return 0;
}

uint32_t font_misses(void)
{
    return misses;
}

const uint8_t *font_glyph(uint32_t c)
{
    if (c >= 0x20 && c < 0x7f)
        return font_ascii + font_ascii_off[c - 0x20];
    int i = ready ? find(c) : -1;
    if (i >= 0 && (cache[i % CACHE].tag == (uint32_t)i + 1 || !fill((uint32_t)i)))
        return cache[i % CACHE].rec;
    return font_ascii + font_ascii_off['?' - 0x20];
}
