/* The library index (library.h, docs/LIBRARY.md). The sort key's tables are the same as in
 * fw/tools/mklib.py: the host test (fw/tools/library_test.c) checks that both sort alike. */
#include <stdint.h>
#include "library.h"

#define HALF   (4u << 20)
#define NSECT  9
enum { S_VOLS, S_STRS, S_TRKS, S_ARTS, S_ALBS, S_GENS, S_GALB, S_SONG, S_CLUS };
static const char sect_id[] = "VOLSSTRSTRKSARTSALBSGENSGALBSONGCLUS";  /* 4 bytes each */
static const uint8_t sect_size[NSECT] = { 12, 0, 36, 8, 16, 12, 2, 4, 8 };

static struct {
    struct lib_io io;
    uint32_t half;                              /* file offset of the half in use */
    uint32_t off[NSECT], len[NSECT], n[NSECT];  /* offsets from the half's start */
    int open;
} lib;

/* ---- reading, through sectors cached: four, or the buffer lib_cache() gives ---- */
#define CACHE 4
#ifdef LIBRARY_HOST
#define SCREENRAM
#else
#define SCREENRAM __attribute__((section(".screenram")))  /* not the BSS: set in lib_open */
#endif
struct ce { uint32_t sector; uint32_t age; uint8_t b[512]; };
static struct ce cache4[CACHE] SCREENRAM, *cache = cache4;
static uint32_t cache_age, ncache = CACHE, last;

static void cache_clear(void)
{
    if (!cache) {
        cache = cache4;
        ncache = CACHE;
    }
    for (uint32_t i = 0; i < ncache; i++)
        cache[i].sector = 0xffffffffu;
    last = 0;
}

void lib_cache(void *buf, uint32_t bytes)
{
    cache = buf && bytes >= sizeof(struct ce) * CACHE ? buf : cache4;
    ncache = cache == cache4 ? CACHE : bytes / sizeof(struct ce);
    cache_clear();
}

static int rd(uint32_t off, void *buf, uint32_t n)
{
    uint8_t *d = buf;
    while (n) {
        uint32_t s = off / 512, o = off % 512, k = 512 - o < n ? 512 - o : n;
        int c = cache[last].sector == s ? (int)last : -1, old = 0;
        for (uint32_t i = 0; c < 0 && i < ncache; i++) {
            if (cache[i].sector == s)
                c = (int)i;
            if (cache[i].age < cache[old].age)
                old = (int)i;
        }
        if (c < 0) {                            /* a miss: the oldest sector replaced */
            c = old;
            if (lib.io.read(lib.io.ctx, s * 512, cache[c].b, 512) < 0) {
                cache[c].sector = 0xffffffffu;
                return -1;
            }
            cache[c].sector = s;
        }
        cache[c].age = ++cache_age;
        last = (uint32_t)c;
        for (uint32_t i = 0; i < k; i++)
            d[i] = cache[c].b[o + i];
        d += k;
        off += k;
        n -= k;
    }
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* ---- CRC-32 (zlib's), four bits at a time ---- */
static const uint32_t crc_t[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158,
    0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4,
    0xa00ae278, 0xbdbdf21c,
};

static uint32_t crc_add(uint32_t c, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        c = (c >> 4) ^ crc_t[c & 15];
        c = (c >> 4) ^ crc_t[c & 15];
    }
    return c;
}

uint32_t lib_crc(uint32_t c, const void *p, uint32_t n)
{
    return crc_add(c, p, n);
}

/* ---- the file on the device ---- */
#ifndef LIBRARY_HOST
#include "fat.h"
#include "fatwrite.h"
#include "log.h"

void lib_empty_header(uint8_t h[512])
{
    for (int i = 0; i < 512; i++)
        h[i] = 0;
    __builtin_memcpy(h, "NEWFWLIB", 8);
    h[8] = 1;                                   /* version 1 */
    h[12] = h[13] = h[14] = h[15] = 0xff;       /* no half in use */
    uint32_t c = ~crc_add(0xffffffffu, h, 508);
    for (int i = 0; i < 4; i++)
        h[508 + i] = (uint8_t)(c >> 8 * i);
}

int lib_file(struct fat_file *f)
{
    static uint8_t h[512] __attribute__((aligned(4))) SCREENRAM;
    lib_empty_header(h);
    int r = fat_create_contig(LIB_FILE, LIB_FILE_SIZE, h, f);
    if (r < 0) {
        log_printf("library: %s not created (%d)", LIB_FILE, r);
        return r;
    }
    uint32_t first, count;
    if (f->size != LIB_FILE_SIZE || fat_extent(f, &first, &count)) {
        log_printf("library: %s is not one (%u bytes%s): left as it is", LIB_FILE, f->size,
                   f->size == LIB_FILE_SIZE ? ", fragmented" : "");
        return LIB_EDAMAGED;
    }
    log_printf("library: %s %s, sectors %x + %u", LIB_FILE, r ? "found" : "created", first, count);
    return 0;
}
#endif

/* ---- opening ---- */
void (*lib_yield)(void);

/* the key of every 64th CLUS entry, in RAM (lib_fence): a search reads one or two sectors */
static uint32_t *fence, nfence;

void lib_close(void)
{
    lib.open = 0;
    fence = 0;
}

int lib_open(const struct lib_io *io)
{
    uint8_t h[512];
    lib.open = 0;
    fence = 0;
    lib.io = *io;
    cache_clear();
    if (rd(0, h, 512) || __builtin_memcmp(h, "NEWFWLIB", 8) || le32(h + 8) != 1)
        return -1;
    if (~crc_add(0xffffffffu, h, 508) != le32(h + 508))
        return -2;
    uint32_t act = le32(h + 12);
    if (act > 1)
        return -1;                              /* none in use (being built) */
    uint32_t len = le32(h + 20 + 8 * act), crc = le32(h + 24 + 8 * act);
    lib.half = 512 + act * HALF;
    if (len < 16 || len > HALF)
        return -2;
    uint32_t c = 0xffffffffu;
    for (uint32_t at = 0; at < len; at += 512) {
        uint8_t b[512];
        uint32_t k = len - at < 512 ? len - at : 512;
        if (lib.io.read(lib.io.ctx, lib.half + at, b, 512) < 0)
            return -1;
        c = crc_add(c, b, k);
        if (lib_yield && at % (64 * 512) == 0)
            lib_yield();
    }
    if (~c != crc)
        return -2;
    uint8_t s[16];
    if (rd(lib.half, s, 16) || __builtin_memcmp(s, "NWLI", 4) || le32(s + 4) != 1 ||
        le32(s + 12) != len)
        return -2;
    uint32_t ns = le32(s + 8);
    for (int k = 0; k < NSECT; k++)
        lib.len[k] = lib.n[k] = lib.off[k] = 0;
    int found = 0;
    for (uint32_t i = 0; i < ns && 16 + 16 * i + 16 <= len; i++) {
        uint8_t e[16];
        if (rd(lib.half + 16 + 16 * i, e, 16))
            return -1;
        for (int k = 0; k < NSECT; k++)
            if (!__builtin_memcmp(e, sect_id + 4 * k, 4)) {  /* others: a later version's, ignored */
                uint32_t o = le32(e + 4), l = le32(e + 8), n = le32(e + 12);
                if (o > len || l > len - o || (o & 3) ||
                    (sect_size[k] && n > l / sect_size[k]))
                    return -2;
                lib.off[k] = o;
                lib.len[k] = l;
                lib.n[k] = n;
                found |= 1 << k;
            }
    }
    if (found != (1 << NSECT) - 1 || !lib.len[S_STRS])
        return -2;
    lib.open = 1;
    return 0;
}

uint32_t lib_count(int what)
{
    if (!lib.open)
        return 0;
    return what == LIB_ARTISTS ? lib.n[S_ARTS] : what == LIB_ALBUMS ? lib.n[S_ALBS] :
           what == LIB_GENRES ? lib.n[S_GENS] : what == LIB_TRACKS ? lib.n[S_TRKS] : 0;
}

/* entry i of section k into b (its size): 0, or -1 out of range */
static int entry(int k, uint32_t i, uint8_t *b)
{
    if (!lib.open || i >= lib.n[k])
        return -1;
    return rd(lib.half + lib.off[k] + i * sect_size[k], b, sect_size[k]);
}

static int str_ok(uint32_t ref)
{
    return ref < lib.len[S_STRS];
}

int lib_artist(uint32_t i, struct lib_artist *a)
{
    uint8_t b[8];
    if (entry(S_ARTS, i, b))
        return -1;
    a->name = le32(b);
    a->first_album = le16(b + 4);
    a->albums = le16(b + 6);
    return str_ok(a->name) && a->first_album + a->albums <= lib.n[S_ALBS] ? 0 : -1;
}

int lib_album(uint32_t i, struct lib_album *a)
{
    uint8_t b[16];
    if (entry(S_ALBS, i, b))
        return -1;
    a->name = le32(b);
    a->first_track = le32(b + 4);
    a->tracks = le16(b + 8);
    a->artist = le16(b + 10);
    a->year = le16(b + 12);
    return str_ok(a->name) && a->first_track <= lib.n[S_TRKS] &&
           a->tracks <= lib.n[S_TRKS] - a->first_track && a->artist < lib.n[S_ARTS] ? 0 : -1;
}

int lib_genre(uint32_t i, struct lib_genre *g)
{
    uint8_t b[12];
    if (entry(S_GENS, i, b))
        return -1;
    g->name = le32(b);
    g->first = le32(b + 4);
    g->albums = le32(b + 8);
    return str_ok(g->name) && g->first <= lib.n[S_GALB] &&
           g->albums <= lib.n[S_GALB] - g->first ? 0 : -1;
}

int lib_genre_album(const struct lib_genre *g, uint32_t k, uint32_t *album)
{
    uint8_t b[2];
    if (k >= g->albums || entry(S_GALB, g->first + k, b))
        return -1;
    *album = le16(b);
    return *album < lib.n[S_ALBS] ? 0 : -1;
}

int lib_song(uint32_t i, uint32_t *track)
{
    uint8_t b[4];
    if (entry(S_SONG, i, b))
        return -1;
    *track = le32(b);
    return *track < lib.n[S_TRKS] ? 0 : -1;
}

int lib_volume(uint32_t i, uint32_t *volume, uint32_t *serial, uint32_t *tracks)
{
    uint8_t b[12];
    if (entry(S_VOLS, i, b))
        return -1;
    *volume = b[0];
    *serial = le32(b + 4);
    *tracks = le32(b + 8);
    return 0;
}

int lib_clus(uint32_t i, uint32_t *key, uint32_t *track)
{
    uint8_t b[8];
    if (entry(S_CLUS, i, b))
        return -1;
    *key = le32(b);
    *track = le32(b + 4);
    return *track < lib.n[S_TRKS] ? 0 : -1;
}

int lib_fence(uint32_t *buf, uint32_t max)
{
    fence = 0;
    uint32_t n = lib.open ? (lib.n[S_CLUS] + 63) / 64 : 0, t;
    if (!buf || !n || n > max)
        return -1;
    for (uint32_t j = 0; j < n; j++)
        if (lib_clus(64 * j, &buf[j], &t))
            return -1;
    fence = buf;
    nfence = n;
    return 0;
}

int lib_find(uint32_t volume, uint32_t cluster, uint32_t *track)
{
    uint32_t want = volume << 31 | cluster, lo = 0, hi = lib.open ? lib.n[S_CLUS] : 0;
    if (fence && hi) {                          /* the last 64 entries starting not above want */
        uint32_t a = 0, z = nfence;
        while (a < z) {
            uint32_t m = (a + z) / 2;
            if (fence[m] <= want)
                a = m + 1;
            else
                z = m;
        }
        if (!a)
            return -1;
        lo = 64 * (a - 1);
        if (hi > lo + 64)
            hi = lo + 64;
    }
    while (lo < hi) {                           /* the first entry not below want */
        uint32_t mid = (lo + hi) / 2, key, t;
        if (lib_clus(mid, &key, &t))
            return -1;
        if (key < want)
            lo = mid + 1;
        else
            hi = mid;
    }
    uint32_t key;
    return lo < (lib.open ? lib.n[S_CLUS] : 0) && !lib_clus(lo, &key, track) && key == want ? 0
                                                                                        : -1;
}

int lib_track(uint32_t i, struct lib_track *t)
{
    uint8_t b[36];
    if (entry(S_TRKS, i, b))
        return -1;
    t->cluster = le32(b);
    t->size = le32(b + 4);
    t->mtime = le32(b + 8);
    t->title = le32(b + 12);
    t->file = le32(b + 16);
    t->artist = le32(b + 20);
    t->album = le16(b + 24);
    t->genre = le16(b + 26);
    t->track = le16(b + 28);
    t->year = le16(b + 30);
    t->disc = b[32];
    t->codec = b[33];
    t->vol = b[34];
    t->contig = b[35] & 1;
    return str_ok(t->title) && str_ok(t->file) && str_ok(t->artist) &&
           t->album < lib.n[S_ALBS] && t->genre < lib.n[S_GENS] ? 0 : -1;
}

int lib_str(uint32_t ref, char *buf, uint32_t max)
{
    if (!lib.open || !str_ok(ref) || !max)
        return -1;
    uint32_t n = 0, end = lib.len[S_STRS];
    uint8_t c = 0;
    while (ref + n < end) {
        if (rd(lib.half + lib.off[S_STRS] + ref + n, &c, 1))
            return -1;
        if (!c || n == max - 1)
            break;
        buf[n++] = (char)c;
    }
    if (c && (c & 0xc0) == 0x80) {              /* cut inside a character: it is dropped */
        while (n && ((uint8_t)buf[n - 1] & 0xc0) == 0x80)
            n--;
        if (n)
            n--;
    }
    buf[n] = 0;
    return (int)n;
}

/* ---- the sort key ---- */
/* U+00C0 to U+017F: two characters each, the base letters ("  ": the character itself) */
static const char latin[] =
    "a a a a a a aec e e e e i i i i d n o o o o o   o u u u u y thssa a a a a a aec e e e "
    "e i i i i d n o o o o o   o u u u u y thy a a a a a a c c c c c c c c d d d d e e e e "
    "e e e e e e g g g g g g g g h h h h i i i i i i i i i i ijijj j k k k l l l l l l l l "
    "l l n n n n n n n n n o o o o o o oeoer r r r r r s s s s s s s s t t t t t t u u u u "
    "u u u u u u u u w w y y y z z z z z z s ";
static const uint16_t greek[][2] = {
    { 0x386, 0x3b1 }, { 0x388, 0x3b5 }, { 0x389, 0x3b7 }, { 0x38a, 0x3b9 }, { 0x38c, 0x3bf },
    { 0x38e, 0x3c5 }, { 0x38f, 0x3c9 }, { 0x3ac, 0x3b1 }, { 0x3ad, 0x3b5 }, { 0x3ae, 0x3b7 },
    { 0x3af, 0x3b9 }, { 0x3cc, 0x3bf }, { 0x3cd, 0x3c5 }, { 0x3ce, 0x3c9 }, { 0x3c2, 0x3c3 },
    { 0x390, 0x3b9 }, { 0x3b0, 0x3c5 }, { 0x3aa, 0x3b9 }, { 0x3ab, 0x3c5 }, { 0x3ca, 0x3b9 },
    { 0x3cb, 0x3c5 },
};

/* The next code point of s (UTF-8; a bad byte as itself) */
static uint32_t next_cp(const uint8_t **s)
{
    const uint8_t *p = *s;
    uint32_t c = *p++;
    int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (more)
        c &= 0x3f >> more;
    for (; more && (*p & 0xc0) == 0x80; more--)
        c = c << 6 | (*p++ & 0x3f);
    *s = p;
    return c;
}

/* A reader of folded code points over a string */
struct key { const uint8_t *s; uint32_t pend; };

static uint32_t key_next(struct key *k)           /* 0 at the end */
{
    if (k->pend) {
        uint32_t c = k->pend;
        k->pend = 0;
        return c;
    }
    if (!*k->s)
        return 0;
    uint32_t c = next_cp(&k->s);
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0xc0 && c <= 0x17f) {
        const char *cell = latin + 2 * (c - 0xc0);
        if (cell[0] == ' ')
            return c;
        if (cell[1] != ' ')
            k->pend = (uint8_t)cell[1];
        return (uint8_t)cell[0];
    }
    for (unsigned i = 0; i < sizeof greek / sizeof *greek; i++)
        if (greek[i][0] == c)
            return greek[i][1];
    if (c >= 0x391 && c <= 0x3a9)
        return c + 32;
    if (c == 0x401 || c == 0x451)
        return 0x435;
    if (c >= 0x410 && c <= 0x42f)
        return c + 32;
    if (c >= 0x400 && c <= 0x40f)
        return c + 80;
    return c;
}

static const uint8_t *skip_the(const uint8_t *s)
{
    if ((s[0] | 32) == 't' && (s[1] | 32) == 'h' && (s[2] | 32) == 'e' && s[3] == ' ' && s[4])
        return s + 4;
    return s;
}

void lib_key(const char *s, int artist, uint16_t *k, int n, int skip)
{
    const uint8_t *p = (const uint8_t *)s;
    if (!*p) {
        for (int i = 0; i < n; i++)
            k[i] = skip ? 0 : 0xffff;
        return;
    }
    struct key kk = { artist ? skip_the(p) : p, 0 };
    for (int i = 0; i < skip && key_next(&kk); i++)
        ;
    for (int i = 0; i < n; i++) {
        uint32_t c = key_next(&kk);
        k[i] = (uint16_t)(c > 0xffff ? 0xffff : c);
    }
}

int lib_cmp(const char *a, const char *b, int artist)
{
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    if (!*pa || !*pb)
        return !*pa - !*pb;                     /* the empty string last */
    struct key ka = { artist ? skip_the(pa) : pa, 0 }, kb = { artist ? skip_the(pb) : pb, 0 };
    for (;;) {
        uint32_t x = key_next(&ka), y = key_next(&kb);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            break;
    }
    for (;; pa++, pb++)                         /* the same keys: the bytes */
        if (*pa != *pb || !*pa)
            return *pa < *pb ? -1 : *pa > *pb ? 1 : 0;
}
