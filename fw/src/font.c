/* Glyphs of Fusion Pixel 12px from the font blob of our firmware image (tools/mkfusion.py).
 * In RAM: the offset of every 16th record (5.3 KiB: 16 bits each, from a base every 64) and
 * the first code point of every 128th run (140 bytes); the run table itself (first code point
 * and first glyph of each run of consecutive codes, 24 KiB) is read from the eMMC when needed,
 * through a cache of 3 sectors (ETUDE §131: until 0.103 the tables took 37 KiB of the BSS).
 * The records live in a cache of 256 (2 ways), by code point: a glyph drawn again reads
 * nothing, a code point the font lacks is kept as '?'. A missing glyph reads its group of 16
 * records (under 800 bytes) in one eMMC command; finding its run, and the code points of the
 * group's other glyphs, costs up to 2 or 3 sectors more on a cold cache, usually none. */
#include <stdint.h>
#include "emmc.h"
#include "font.h"
#include "log.h"

#define RANGES_MAX 4400
#define CHUNK      128                          /* runs per entry of top[] (one 512-byte sector) */
#define TOP_MAX    ((RANGES_MAX + CHUNK - 1) / CHUNK)
#define GROUPS_MAX 2600
#define GROUP      16
#define REC_MAX    52                           /* largest record: 49 bytes */
#define CACHE      256
#define SECS       3                            /* sectors of the tables kept */

static uint32_t top[TOP_MAX];                   /* first[CHUNK * k] */
/* group[g] (the offset of record 16 g) = gbase[g / GBASE] + gdelta[g]: 64 groups of 16 records
 * of at most REC_MAX bytes span less than 64 KiB */
#define GBASE      64
static uint32_t gbase[(GROUPS_MAX + GBASE - 1) / GBASE];
static uint16_t gdelta[GROUPS_MAX];
static uint32_t nranges, nglyphs, ngroups, ntop, base, blen, o_first, o_index, o_group, o_recs,
                recs_len;
static int ready;

/* 2 ways of 128 sets, by code point (set = cp % SETS, at 2 set and 2 set + 1): two code points
 * of the same set, drawn together, no longer chase each other out (夜 U+591C and 서 U+C11C) */
#define SETS       (CACHE / 2)
static struct { uint32_t tag; uint8_t rec[REC_MAX]; } cache[CACHE];   /* tag = code point + 1 */
static uint8_t mru[SETS];                       /* the way used last in each set */
static uint8_t used[CACHE];                     /* drawn since it came in (not only read ahead) */
static uint32_t sec_lba[SECS], sec_next;        /* lba 0: empty (the blob is past FW1) */
static uint32_t sec[SECS][512 / 4];
static uint32_t rbuf[3 * 512 / 4];              /* a group's records (under 800 bytes) */
static uint32_t reads;

/* bytes [off, off+n) of the blob into dst, through the sector cache: 0, or -1 */
static int rd(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    if (off > blen || n > blen - off)
        return -1;
    while (n) {
        uint32_t lba = base + off / 512, s = off % 512, k = 512 - s, j = 0;
        while (j < SECS && sec_lba[j] != lba)
            j++;
        if (j == SECS) {
            j = sec_next++ % SECS;
            sec_lba[j] = 0;
            if (emmc_read(lba, 1, sec[j]))
                return -1;
            sec_lba[j] = lba;
            reads++;
        }
        if (k > n)
            k = n;
        for (uint32_t i = 0; i < k; i++)
            d[i] = ((const uint8_t *)sec[j])[s + i];
        d += k;
        off += k;
        n -= k;
    }
    return 0;
}

static uint32_t rd32(uint32_t off, int *err)    /* *err set on a read error */
{
    uint32_t v = 0;
    if (rd(off, &v, 4))
        *err = 1;
    return v;
}

int font_init(void)
{
    static uint32_t h[128] __attribute__((section(".screenram")));  /* (read once: not the BSS) */
    for (int k = 0; k < 2; k++) {
        uint32_t img = EMMC_FW1 + (k ? emmc.r : 0);     /* FW1, then FW2 */
        const uint8_t *hb = (const uint8_t *)h;
        if (emmc_read(img, 1, h) || hb[0x1f8] != 'R' || hb[0x1ff] != 'W')
            continue;
        uint32_t off = h[0x80 / 4], len = h[0x84 / 4];
        if (!len || off % 512 || emmc_read(img + off / 512, 1, h) || h[0] != 0x46585046 ||
            h[1] != 1)                          /* "FPXF", version 1 */
            continue;
        ready = 0;
        for (int j = 0; j < SECS; j++)
            sec_lba[j] = 0;
        base = img + off / 512;
        blen = len;
        nglyphs = h[2];
        nranges = h[3];
        ngroups = h[10];
        o_first = h[5];
        o_index = h[6];
        o_group = h[7];
        o_recs = h[8];
        recs_len = h[9];
        ntop = (nranges + CHUNK - 1) / CHUNK;
        int err = 0;
        if (!nranges || nranges > RANGES_MAX || ngroups > GROUPS_MAX || o_recs > len ||
            recs_len > len - o_recs || o_first > len || 4 * nranges > len - o_first ||
            o_index > len || 2 * (nranges + 1) > len - o_index || o_group > len ||
            4 * ngroups > len - o_group)
            continue;
        for (uint32_t t = 0; t < ntop && !err; t++)
            top[t] = rd32(o_first + 4 * CHUNK * t, &err);
        for (uint32_t g = 0, prev = 0; g < ngroups && !err; g++) {
            uint32_t v = rd32(o_group + 4 * g, &err);
            if (g % GBASE == 0)
                gbase[g / GBASE] = v;
            if (v < prev || v > recs_len || v - gbase[g / GBASE] > 0xffff)
                err = 1;                        /* (not made by mkfusion.py) */
            gdelta[g] = (uint16_t)(v - gbase[g / GBASE]);
            prev = v;
        }
        if (err)
            continue;
        ready = 1;
        log_printf("font: Fusion Pixel 12px from %s, %u glyphs, %u runs", k ? "FW2" : "FW1",
                   nglyphs, nranges);
        return 0;
    }
    log_printf("font: no font blob in FW1 or FW2: ASCII only");
    return -1;
}

/* run r: its first code point *f and its glyphs [*a, *b): 0, or -1 */
static int run(uint32_t r, uint32_t *f, uint32_t *a, uint32_t *b)
{
    int err = 0;
    uint16_t ix[2];
    *f = rd32(o_first + 4 * r, &err);
    if (err || r >= nranges || rd(o_index + 2 * r, ix, 4) || ix[0] > ix[1] || ix[1] > nglyphs)
        return -1;
    *a = ix[0];
    *b = ix[1];
    return 0;
}

/* the glyph of c: its index *i and its run *r; 0, 1 when the font lacks c, -1 on a read error */
static int find(uint32_t c, uint32_t *i, uint32_t *r)
{
    if (c < top[0])
        return 1;
    uint32_t lo = 0, hi = ntop;                 /* the chunk: top[lo] <= c < top[lo + 1] */
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (top[mid] <= c)
            lo = mid;
        else
            hi = mid;
    }
    int err = 0;
    hi = lo + 1 < ntop ? (lo + 1) * CHUNK : nranges;    /* then the run, inside the chunk */
    lo *= CHUNK;
    while (hi - lo > 1 && !err) {
        uint32_t mid = (lo + hi) / 2;
        if (rd32(o_first + 4 * mid, &err) <= c)
            lo = mid;
        else
            hi = mid;
    }
    uint32_t f, a, b;
    if (err || run(lo, &f, &a, &b))
        return -1;
    if (c - f >= b - a)
        return 1;
    *i = a + (c - f);
    *r = lo;
    return 0;
}

/* the cache entry of cp, or -1 */
static int lookup(uint32_t cp)
{
    uint32_t k = cp % SETS * 2;
    for (int w = 0; w < 2; w++)
        if (cache[k + w].tag == cp + 1) {
            mru[cp % SETS] = (uint8_t)w;
            used[k + w] = 1;
            return (int)(k + w);
        }
    return -1;
}

/* cp's record (n bytes) into the cache. Asked for (ahead = 0): over the way used least. Read
 * ahead with it (the rest of its group, ahead = 1): only into a free way or one read ahead and
 * never drawn, so that a line drawn again and again keeps its glyphs (ETUDE §132). NULL: not
 * kept. */
static uint8_t *put(uint32_t cp, const uint8_t *rec, uint32_t n, int ahead)
{
    uint32_t k0 = cp % SETS * 2;
    int k = -1;
    for (int w = 0; w < 2 && k < 0; w++)
        if (cache[k0 + w].tag == cp + 1)
            k = (int)k0 + w;
    if (k < 0 && !ahead)
        k = (int)k0 + !mru[cp % SETS];
    for (int w = 0; w < 2 && k < 0; w++)
        if (!cache[k0 + w].tag || !used[k0 + w])
            k = (int)k0 + w;
    if (k < 0)
        return 0;
    if (!ahead) {
        mru[cp % SETS] = (uint8_t)(k & 1);
        used[k] = 1;
    } else if (cache[k].tag != cp + 1) {
        used[k] = 0;
    }
    for (uint32_t x = 0; x < n; x++)
        cache[k].rec[x] = rec[x];
    cache[k].tag = cp + 1;
    return cache[k].rec;
}

/* reads the group of glyph i (code point c, in run r) into the cache, in one eMMC command, each
 * glyph by its code point (found in the runs around r); c's last, so that it is kept */
static int fill(uint32_t i, uint32_t c, uint32_t r)
{
    uint32_t g = i / GROUP, j = g * GROUP, f, a, b, ni = 0;
    const uint8_t *qi = 0;
    if (g >= ngroups)
        return -1;
    uint32_t p = gbase[g / GBASE] + gdelta[g];
    uint32_t e = g + 1 < ngroups ? gbase[(g + 1) / GBASE] + gdelta[g + 1] : recs_len;
    if (p > e || e > recs_len)
        return -1;
    uint32_t start = o_recs + p, sec0 = start / 512, nsec = (o_recs + e + 511) / 512 - sec0;
    if (nsec > 3 || emmc_read(base + sec0, nsec, rbuf))
        return -1;
    reads++;
    if (run(r, &f, &a, &b))
        return -1;
    while (a > j && r) {                        /* the run of the group's first glyph */
        if (run(--r, &f, &a, &b))
            return -1;
    }
    const uint8_t *q = (const uint8_t *)rbuf + start % 512;
    for (; j < nglyphs && j < (g + 1) * GROUP; j++) {
        while (j >= b)                          /* (runs are never empty: mkfusion.py) */
            if (++r >= nranges || run(r, &f, &a, &b))
                return -1;
        uint32_t n = (uint32_t)(5 + q[2] * ((q[1] + 7) / 8));
        if (p + n > e || n > REC_MAX || j < a)
            return -1;
        if (j == i) {
            qi = q;
            ni = n;
        } else {
            put(f + (j - a), q, n, 1);
        }
        q += n;
        p += n;
    }
    if (!qi)
        return -1;
    put(c, qi, ni, 0);
    return 0;
}

uint32_t font_misses(void)
{
    return reads;
}

const uint8_t *font_glyph(uint32_t c)
{
    const uint8_t *q = font_ascii + font_ascii_off['?' - 0x20];
    if (c >= 0x20 && c < 0x7f)
        return font_ascii + font_ascii_off[c - 0x20];
    if (!ready || c > 0x10ffff)
        return q;
    uint32_t i, r;
    int k = lookup(c);                          /* (a code point the font lacks too: '?') */
    if (k >= 0)
        return cache[k].rec;
    int found = find(c, &i, &r);
    if (found == 1)                             /* not in the font: '?' kept as c's glyph */
        return put(c, q, (uint32_t)(5 + q[2] * ((q[1] + 7) / 8)), 0);
    if (found == 0 && !fill(i, c, r) && (k = lookup(c)) >= 0)
        return cache[k].rec;
    return q;
}
