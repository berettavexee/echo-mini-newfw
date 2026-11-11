/* Host-side test of src/font.c (ETUDE §131): the font blob of the build (tools/mkfusion.py)
 * served as the eMMC's FW1, every code point of the font asked in a shuffled order (the caches
 * evicted again and again) and its record compared with the one the blob gives, read here
 * directly; code points the font lacks give '?'; then the same with eMMC reads failing at
 * random: a glyph is right or '?', never another one; a damaged header is refused.
 *   cc -O1 -Wall -Isrc -Ibuild -o build/font_test tools/font_test.c && \
 *       build/font_test build/font.bin */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/font.c"
#include "font_ascii.c"

struct emmc_geom emmc = { .p = 0x10000, .r = 0x8000 };

#define BLOB_OFF 0x1000u                        /* the blob, in the image */
static uint8_t *img;                            /* FW1: header, then the blob */
static uint32_t img_n;
static int fail_pct;                            /* reads that fail, % */
static uint32_t n_reads, n_rec, rec_sec0;  /* n_rec: reads in the records */

int emmc_read(uint32_t sector, uint32_t count, void *buf)
{
    n_reads++;
    n_rec += rec_sec0 && sector >= rec_sec0;
    if (fail_pct && rand() % 100 < fail_pct)
        return EMMC_EDATA;
    if (sector < EMMC_FW1 || (sector - EMMC_FW1 + count) * 512 > img_n) {
        memset(buf, 0, 512 * count);            /* (FW2 and beyond: nothing) */
        return 0;
    }
    memcpy(buf, img + (sector - EMMC_FW1) * 512, 512 * count);
    return 0;
}

void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static int is_q(const uint8_t *g)               /* the record of '?' (a copy kept, or ASCII's) */
{
    const uint8_t *q = font_ascii + font_ascii_off['?' - 0x20];
    return !memcmp(g, q, 5 + q[2] * ((q[1] + 7) / 8));
}

static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f) {
        fprintf(stderr, "usage: font_test build/font.bin\n");
        return 2;
    }
    static uint8_t blob[4 << 20];
    uint32_t blen = (uint32_t)fread(blob, 1, sizeof blob, f);
    fclose(f);
    img_n = (BLOB_OFF + blen + 511) & ~511u;
    img = calloc(1, img_n);
    img[0x1f8] = 'R';
    img[0x1ff] = 'W';
    memcpy(img + 0x80, &(uint32_t){ BLOB_OFF }, 4);
    memcpy(img + 0x84, &blen, 4);
    memcpy(img + BLOB_OFF, blob, blen);

    /* the reference: code point and record of every glyph, from the blob */
    uint32_t ng = g32(blob + 8), nr = g32(blob + 12), of = g32(blob + 0x14), oi = g32(blob + 0x18),
             orec = g32(blob + 0x20);
    uint32_t *cp = malloc(4 * ng), *roff = malloc(4 * ng);
    for (uint32_t r = 0; r < nr; r++) {
        uint32_t i0 = blob[oi + 2 * r] | blob[oi + 2 * r + 1] << 8;
        uint32_t i1 = blob[oi + 2 * r + 2] | blob[oi + 2 * r + 3] << 8;
        for (uint32_t i = i0; i < i1; i++)
            cp[i] = g32(blob + of + 4 * r) + (i - i0);
    }
    for (uint32_t i = 0, p = orec; i < ng; i++) {
        roff[i] = p;
        p += 5 + blob[p + 2] * ((blob[p + 1] + 7) / 8);
    }
    const uint8_t *qmark = font_ascii + font_ascii_off['?' - 0x20];
    int fails = 0;
    if (font_init()) {
        printf("FAIL font_init\n");
        return 1;
    }
    printf("RAM: %zu bytes of tables, %zu of sector buffers\n", sizeof top + sizeof gbase +
           sizeof gdelta, sizeof sec + sizeof rbuf);
    uint32_t *order = malloc(4 * ng);
    for (int pass = 0; pass < 3; pass++) {
        fail_pct = pass == 2 ? 20 : 0;
        for (uint32_t i = 0; i < ng; i++)
            order[i] = i;
        for (uint32_t i = ng - 1; i > 0; i--) {
            uint32_t j = (uint32_t)rand() % (i + 1), t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        uint32_t r0 = n_reads, wrong = 0, q = 0;
        for (uint32_t k = 0; k < ng; k++) {
            uint32_t i = order[k];
            if (cp[i] >= 0x20 && cp[i] < 0x7f)
                continue;
            const uint8_t *g = font_glyph(cp[i]), *ref = blob + roff[i];
            uint32_t n = 5 + ref[2] * ((ref[1] + 7) / 8);
            if (g == qmark && fail_pct && !is_q(ref))
                q++;
            else if (memcmp(g, ref, n)) {
                if (wrong++ < 5)
                    printf("FAIL pass %d: U+%04X differs\n", pass, cp[i]);
            }
        }
        /* absent code points: between runs, before the first, beyond the last */
        uint32_t absent[] = { 0x7f, 0x80, 0x10ffff, 0xffffffff };
        for (uint32_t k = 0; k < 4; k++)
            if (!is_q(font_glyph(absent[k])) && !fail_pct && wrong++ < 5)
                printf("FAIL pass %d: absent U+%X not '?'\n", pass, absent[k]);
        for (uint32_t r = 0; r + 1 < nr && !fail_pct; r++) {
            uint32_t c = g32(blob + of + 4 * r) + (blob[oi + 2 * r + 2] | blob[oi + 2 * r + 3] << 8) -
                         (blob[oi + 2 * r] | blob[oi + 2 * r + 1] << 8);   /* just after run r */
            if (c < g32(blob + of + 4 * (r + 1)) && !is_q(font_glyph(c)) && wrong++ < 5)
                printf("FAIL pass %d: absent U+%04X not '?'\n", pass, c);
        }
        printf("%s pass %d (%s): %u glyphs, %u eMMC reads, %u '?' from failed reads\n",
               wrong ? "FAIL" : "ok  ", pass, pass == 2 ? "20 % of reads failing" :
               pass ? "warm" : "cold", ng, n_reads - r0, q);
        fails += wrong != 0;
    }
    /* real text (argv[2], UTF-8 lines, e.g. a catalogue): eMMC reads to draw it the first and
     * the second time, against the tables in RAM until 0.103 (one read per group missing in a
     * cache of 256 glyphs by index) */
    FILE *t = argc > 2 ? fopen(argv[2], "rb") : NULL;
    if (t) {
        static uint32_t old_tag[256];
        static char line[4096];
        rec_sec0 = EMMC_FW1 + (BLOB_OFF + orec) / 512 + 1;
        uint32_t chars = 0, r_new[2] = { 0, 0 }, r_old[2] = { 0, 0 };
        fail_pct = 0;
        for (int j = 0; j < SECS; j++)                  /* everything cold */
            sec_lba[j] = 0;
        for (int j = 0; j < CACHE; j++)
            cache[j].tag = 0;
        font_init();
        for (int round = 0; round < 2; round++) {
            rewind(t);
            while (fgets(line, sizeof line, t)) {
                for (const unsigned char *u = (const unsigned char *)line; *u;) {
                    uint32_t c = *u++;
                    if (c >= 0xc0) {
                        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
                        c &= 0x3f >> more;
                        while (more-- && (*u & 0xc0) == 0x80)
                            c = c << 6 | (*u++ & 0x3f);
                    }
                    if (c < 0x7f)
                        continue;
                    chars += !round;
                    uint32_t r0 = n_reads;
                    font_glyph(c);
                    r_new[round] += n_reads - r0;
                    uint32_t lo = 0, hi = ng;                   /* the old cache, by index */
                    while (hi - lo > 1)
                        if (cp[(lo + hi) / 2] <= c)
                            lo = (lo + hi) / 2;
                        else
                            hi = (lo + hi) / 2;
                    if (cp[lo] == c && old_tag[lo % 256] != lo + 1) {
                        r_old[round]++;
                        for (uint32_t j = lo / 16 * 16; j < ng && j < lo / 16 * 16 + 16; j++)
                            old_tag[j % 256] = j + 1;
                    }
                }
            }
        }
        fclose(t);
        printf("text %s: %u non-ASCII characters; eMMC reads: first drawing %u (until 0.103: "
               "%u), second %u (%u); %u of them in the records\n", argv[2], chars, r_new[0],
               r_old[0], r_new[1], r_old[1], n_rec);
    }

    /* a line drawn again and again (a scrolling title, 30 times a second): read once, then
     * never. Two of its code points share a cache set (夜 U+591C, 서 U+C11C); the groups read
     * for its glyphs bring their neighbours, which must not push its glyphs out (ETUDE §132:
     * 東京の夜 龍與鳳凰 서울의 밤 ひらがな・カタカナ, the screen test's line) */
    fail_pct = 0;
    {
        static const uint32_t line[] = { 0x6771, 0x4eac, 0x306e, 0x591c, 0x9f8d, 0x8207, 0x9cf3,
                                         0x51f0, 0xc11c, 0xc6b8, 0xc758, 0xbc24, 0x3072, 0x3089,
                                         0x304c, 0x306a, 0x30fb, 0x30ab, 0x30bf, 0x30ab, 0x30ca };
        for (uint32_t k = 0; k < sizeof line / 4; k++)
            font_glyph(line[k]);
        uint32_t r0 = n_reads;
        for (int round = 0; round < 30; round++)
            for (uint32_t k = 0; k < sizeof line / 4; k++)
                if (is_q(font_glyph(line[k]))) {
                    printf("FAIL U+%04X gives '?'\n", line[k]);
                    fails++;
                }
        printf("%s a CJK line drawn 30 times more: %u eMMC reads\n", n_reads == r0 ? "ok  " : "FAIL",
               n_reads - r0);
        fails += n_reads != r0;
    }

    /* a damaged header: refused, ASCII only */
    fail_pct = 0;
    memcpy(img + BLOB_OFF + 12, &(uint32_t){ 100000 }, 4);           /* runs > RANGES_MAX */
    if (!font_init() || !is_q(font_glyph(cp[ng - 1]))) {
        printf("FAIL damaged header accepted\n");
        fails++;
    }
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
