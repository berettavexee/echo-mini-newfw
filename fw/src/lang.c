/* The interface's languages (ETUDE §112): see i18n.h. The catalogues (tools/mklang.py, from
 * po/<language>.po) are packed in our firmware image after the font, header field 0x78 (the
 * stock 12 px font slot, empty in our images). The directory of the pack (codes and names) is
 * read at boot; the catalogue of the language chosen is read into a buffer of the screens' RAM
 * (section .screenram, not the BSS), and tr() looks the English strings up in it by their hash.
 *
 * Pack (little-endian): "NWLP", u32 version 1, u32 count, u32 0, then count entries of
 *   char code[8], char name[24] (UTF-8, NUL-padded), u32 offset (from the pack), u32 length.
 * Catalogue: "NWLC", u32 version 1, u32 n, u32 0, then n of {u32 hash, u32 offset of the
 *   translation from the catalogue's start}, sorted by hash, then the translations, UTF-8,
 *   NUL-terminated. hash: FNV-1a 32 of the English string's bytes. */
#include <stdint.h>
#include "emmc.h"
#include "i18n.h"
#include "lang.h"
#include "log.h"

#define CAT_MAX  0x4000                         /* 16 KiB: a larger catalogue is not loaded */

struct lang_ent {
    char code[8], name[24];
    uint32_t off, len;
};
static struct lang_ent dir[LANG_MAX] __attribute__((section(".screenram")));
static int n_dir, cur;                          /* cur: 0 English, else dir[cur - 1] */
static uint32_t pack_lba;
static uint32_t cat[CAT_MAX / 4] __attribute__((section(".screenram")));
static uint32_t cat_n;

static uint32_t sec[512 / 4] __attribute__((section(".screenram")));   /* (not the BSS) */

/* bytes [off, off + n) of the pack into dst */
static int pack_read(uint32_t off, void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n) {
        uint32_t s = off % 512, k = 512 - s;
        if (k > n)
            k = n;
        if (emmc_read(pack_lba + off / 512, 1, sec))
            return -1;
        for (uint32_t i = 0; i < k; i++)
            d[i] = ((const uint8_t *)sec)[s + i];
        d += k;
        off += k;
        n -= k;
    }
    return 0;
}

int lang_init(void)
{
    n_dir = cur = 0;
    for (int k = 0; k < 2; k++) {                   /* FW1, then FW2 (as the font) */
        uint32_t len;
        if (fw_blob(k, 0x78, 0x504c574e, sec, &pack_lba, &len) || sec[1] != 1)  /* "NWLP" */
            continue;
        uint32_t n = sec[2] < LANG_MAX ? sec[2] : LANG_MAX;
        if (pack_read(16, dir, n * sizeof *dir))
            continue;
        for (uint32_t i = 0; i < n; i++) {
            dir[i].code[7] = dir[i].name[23] = 0;
            if (dir[i].off + dir[i].len > len)
                dir[i].len = 0;                 /* outside the pack: never loaded */
        }
        n_dir = (int)n;
        log_printf("lang: %d catalogue(s) in %s", n_dir, k ? "FW2" : "FW1");
        return 0;
    }
    log_printf("lang: no catalogue: English only");
    return -1;
}

int lang_count(void)
{
    return 1 + n_dir;
}

const char *lang_code(int i)
{
    return i <= 0 || i > n_dir ? "en" : dir[i - 1].code;
}

const char *lang_name(int i)
{
    /* TRANSLATORS: the name of this language, written in it (Français, 简体中文, 日本語): shown by
       the Language setting */
    return i <= 0 || i > n_dir ? N_("English") : dir[i - 1].name;
}

int lang_current(void)
{
    return cur;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

int lang_set(int i)
{
    cur = 0;
    cat_n = 0;
    if (i <= 0 || i > n_dir)
        return 0;
    const struct lang_ent *e = &dir[i - 1];
    if (e->len < 16 || e->len > CAT_MAX || pack_read(e->off, cat, e->len) ||
        cat[0] != 0x434c574e || cat[1] != 1 || 16 + 8 * cat[2] > e->len) {     /* "NWLC" */
        log_printf("lang: %s: catalogue not loaded (%u bytes): English", e->code, e->len);
        return -1;
    }
    ((uint8_t *)cat)[e->len - 1] = 0;           /* the last string ends in the buffer */
    for (uint32_t k = 0; k < cat[2]; k++)
        if (cat[4 + 2 * k + 1] >= e->len) {
            log_printf("lang: %s: damaged catalogue: English", e->code);
            return -1;
        }
    cat_n = cat[2];
    cur = i;
    log_printf("lang: %s (%s), %u strings, %u bytes", e->code, e->name, cat_n, e->len);
    return 0;
}

int lang_set_code(const char *code)
{
    for (int i = 1; i <= n_dir; i++)
        if (same(dir[i - 1].code, code))
            return lang_set(i);
    return lang_set(0);
}

const char *tr(const char *s)
{
    if (!cat_n)
        return s;
    uint32_t h = 0x811c9dc5;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++)
        h = (h ^ *p) * 0x01000193;
    uint32_t lo = 0, hi = cat_n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, m = cat[4 + 2 * mid];
        if (m == h)
            return (const char *)cat + cat[4 + 2 * mid + 1];
        if (m < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return s;
}
