/* The library's views (libview.h, ETUDE §127): bitmaps of what can be shown, worked out once
 * from the index (lv_setup), and the i-th item of a list found from them. A track is hidden when
 * it is on a card that is not in, or when it is the card's copy of the track just before or
 * after it in its album (TRKS order: album, disc, number, file name, volume): the same disc,
 * number and title (the file names when a title is empty) on the two volumes. An album shows
 * when one of its tracks does; an artist or a genre when one of its albums does. The tables are
 * in the screens' RAM (.screenram, not the BSS). */
#include "libview.h"
#include "library.h"

#ifdef LIBRARY_HOST
#define LV_RAM
#else
#define LV_RAM __attribute__((section(".screenram")))
#endif

#define MAX_TRACKS  24576u                      /* (libbuild.c's) */
#define MAX_ALBUMS  24576u
#define MAX_ARTISTS 8192u
#define MAX_GENRES  2048u
#define MAX_YEARS   256u
#define MAX_LIST    2048u                       /* the albums of a year or of a genre */

static uint8_t hid[MAX_TRACKS / 8] LV_RAM;      /* tracks hidden */
static uint8_t svis[MAX_TRACKS / 8] LV_RAM;     /* SONG's places shown */
static uint8_t avis[MAX_ALBUMS / 8] LV_RAM;
static uint8_t rvis[MAX_ARTISTS / 8] LV_RAM;
static uint8_t gvis[MAX_GENRES / 8] LV_RAM;
static uint16_t spre[MAX_TRACKS / 64] LV_RAM;   /* the shown before each block of 64 */
static uint16_t apre[MAX_ALBUMS / 64] LV_RAM;
static uint16_t rpre[MAX_ARTISTS / 64] LV_RAM;
static uint16_t gpre[MAX_GENRES / 64] LV_RAM;
static uint16_t years[MAX_YEARS] LV_RAM;
static uint16_t list[MAX_LIST] LV_RAM;          /* the last list of a year or a genre */

static struct {
    int ready, list_kind;
    uint32_t list_param, list_n;
    uint32_t ntr, nal, nar, ngen, nyears;
    uint32_t vis_songs, vis_albums, vis_artists, vis_genres;
} lv;

void (*lv_yield)(void);

static int bit(const uint8_t *b, uint32_t i) { return b[i / 8] >> (i % 8) & 1; }
static void set(uint8_t *b, uint32_t i) { b[i / 8] |= (uint8_t)(1 << (i % 8)); }

static void fill(uint8_t *b, uint32_t n, int v)
{
    for (uint32_t i = 0; i < (n + 7) / 8; i++)
        b[i] = v ? 0xff : 0;
}

/* pre[k]: the bits set in [0, 64k); the count of all of them */
static uint32_t prefix(const uint8_t *b, uint16_t *pre, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i % 64 == 0)
            pre[i / 64] = (uint16_t)c;
        c += (uint32_t)bit(b, i);
    }
    return c;
}

/* The place of the i-th bit set (from 0), or -1 */
static int32_t nth_set(const uint8_t *b, const uint16_t *pre, uint32_t n, uint32_t i)
{
    uint32_t lo = 0, hi = (n + 63) / 64;        /* the last block starting with <= i before */
    if (!hi)
        return -1;
    while (hi - lo > 1) {
        uint32_t m = (lo + hi) / 2;
        if (pre[m] <= i)
            lo = m;
        else
            hi = m;
    }
    uint32_t c = pre[lo];
    for (uint32_t p = 64 * lo; p < n && p < 64 * lo + 64; p++)
        if (bit(b, p) && c++ == i)
            return (int32_t)p;
    return -1;
}

/* the same track on both volumes: its disc, number and title (or file name) */
static int same_track(const struct lib_track *a, const struct lib_track *b)
{
    static char x[128], y[128];
    if (a->album != b->album || a->vol == b->vol || a->disc != b->disc || a->track != b->track)
        return 0;
    int titled = a->title && b->title;
    if (lib_str(titled ? a->title : a->file, x, sizeof x) < 0 ||
        lib_str(titled ? b->title : b->file, y, sizeof y) < 0)
        return 0;
    for (int i = 0;; i++) {
        if (x[i] != y[i])
            return 0;
        if (!x[i])
            return 1;
    }
}

uint32_t lv_card_serial(void)
{
    uint32_t v, s, n;
    for (uint32_t i = 0; !lib_volume(i, &v, &s, &n); i++)
        if (v == 1 && n)
            return s;
    return 0;
}

void lv_reset(void)
{
    lv.ready = 0;
    lv.list_kind = -1;
}

int lv_ready(void)
{
    return lv.ready;
}

static int setup(int card_in)
{
    lv.ntr = lib_count(LIB_TRACKS);
    lv.nal = lib_count(LIB_ALBUMS);
    lv.nar = lib_count(LIB_ARTISTS);
    lv.ngen = lib_count(LIB_GENRES);
    if (lv.ntr > MAX_TRACKS || lv.nal > MAX_ALBUMS || lv.nar > MAX_ARTISTS || lv.ngen > MAX_GENRES)
        return -1;
    uint32_t v, s, n, on_card = 0;
    for (uint32_t i = 0; !lib_volume(i, &v, &s, &n); i++)
        if (v == 1)
            on_card += n;
    fill(hid, lv.ntr, 0);
    if (on_card) {                              /* tracks hidden: the card's, its copies */
        struct lib_track p, t;
        for (uint32_t k = 0; k < lv.ntr; k++) {
            if (lib_track(k, &t))
                return -1;
            if (t.vol == 1 && !card_in)
                set(hid, k);
            else if (k && same_track(&p, &t))
                set(hid, t.vol == 1 ? k : k - 1);
            p = t;
            if (k % 512 == 0 && lv_yield)
                lv_yield();
        }
    }
    fill(avis, lv.nal, 0);
    for (uint32_t i = 0; i < lv.nal; i++) {
        struct lib_album a;
        if (lib_album(i, &a))
            return -1;
        for (uint32_t k = a.first_track; k < a.first_track + a.tracks && k < lv.ntr; k++)
            if (!bit(hid, k)) {
                set(avis, i);
                break;
            }
    }
    fill(rvis, lv.nar, 0);
    for (uint32_t i = 0; i < lv.nar; i++) {
        struct lib_artist a;
        if (lib_artist(i, &a))
            return -1;
        for (uint32_t k = a.first_album; k < a.first_album + a.albums && k < lv.nal; k++)
            if (bit(avis, k)) {
                set(rvis, i);
                break;
            }
    }
    fill(gvis, lv.ngen, 0);
    for (uint32_t i = 0; i < lv.ngen; i++) {
        struct lib_genre g;
        if (lib_genre(i, &g))
            return -1;
        for (uint32_t k = 0; k < g.albums; k++) {
            uint32_t al;
            if (lib_genre_album(&g, k, &al))
                return -1;
            if (al < lv.nal && bit(avis, al)) {
                set(gvis, i);
                break;
            }
        }
    }
    fill(svis, lv.ntr, 1);
    if (on_card)
        for (uint32_t p = 0; p < lv.ntr; p++) {
            uint32_t t;
            if (lib_song(p, &t) || t >= lv.ntr)
                return -1;
            if (bit(hid, t))
                svis[p / 8] &= (uint8_t)~(1 << (p % 8));
        }
    lv.nyears = 0;                              /* the years: ascending, 0 last */
    for (uint32_t i = 0; i < lv.nal; i++) {
        struct lib_album a;
        if (!bit(avis, i) || lib_album(i, &a))
            continue;
        uint32_t y = a.year, at = 0;
        while (at < lv.nyears && (years[at] ? years[at] : 0x10000u) < (y ? y : 0x10000u))
            at++;
        if ((at < lv.nyears && years[at] == y) || lv.nyears == MAX_YEARS)
            continue;
        for (uint32_t k = lv.nyears; k > at; k--)
            years[k] = years[k - 1];
        years[at] = (uint16_t)y;
        lv.nyears++;
    }
    lv.vis_songs = prefix(svis, spre, lv.ntr);
    lv.vis_albums = prefix(avis, apre, lv.nal);
    lv.vis_artists = prefix(rvis, rpre, lv.nar);
    lv.vis_genres = prefix(gvis, gpre, lv.ngen);
    return 0;
}

int lv_setup(int card_in)
{
    lv_reset();
    if (setup(card_in))
        return -1;
    lv.ready = 1;
    return 0;
}

/* The albums of a year or a genre, into list (kept for the next calls) */
static int make_list(int kind, uint32_t param)
{
    if (lv.list_kind == kind && lv.list_param == param)
        return 0;
    lv.list_kind = -1;
    lv.list_n = 0;
    if (kind == LV_YEAR_ALBUMS) {
        for (uint32_t i = 0; i < lv.nal && lv.list_n < MAX_LIST; i++) {
            struct lib_album a;
            if (!bit(avis, i))
                continue;
            if (lib_album(i, &a))
                return -1;
            if (a.year == param)
                list[lv.list_n++] = (uint16_t)i;
        }
    } else {
        struct lib_genre g;
        if (param >= lv.ngen || lib_genre(param, &g))
            return -1;
        for (uint32_t k = 0; k < g.albums && lv.list_n < MAX_LIST; k++) {
            uint32_t al;
            if (lib_genre_album(&g, k, &al))
                return -1;
            if (al < lv.nal && bit(avis, al))
                list[lv.list_n++] = (uint16_t)al;
        }
    }
    lv.list_kind = kind;
    lv.list_param = param;
    return 0;
}

/* The shown among [first, first + count) of a bitmap (hid: the ones not set): their count, or
 * the place of the i-th */
static uint32_t range_count(const uint8_t *b, int want, uint32_t first, uint32_t count,
                            uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t k = first; k < first + count && k < n; k++)
        c += (uint32_t)(bit(b, k) == want);
    return c;
}

static int32_t range_at(const uint8_t *b, int want, uint32_t first, uint32_t count, uint32_t n,
                        uint32_t i)
{
    for (uint32_t k = first; k < first + count && k < n; k++)
        if (bit(b, k) == want && !i--)
            return (int32_t)k;
    return -1;
}

uint32_t lv_count(int kind, uint32_t param)
{
    if (!lv.ready)
        return 0;
    switch (kind) {
    case LV_ARTISTS:
        return lv.vis_artists;
    case LV_ALBUMS:
        return lv.vis_albums;
    case LV_YEARS:
        return lv.nyears;
    case LV_GENRES:
        return lv.vis_genres;
    case LV_SONGS:
        return lv.vis_songs;
    case LV_ARTIST_ALBUMS: {
        struct lib_artist a;
        return param < lv.nar && !lib_artist(param, &a) ?
               range_count(avis, 1, a.first_album, a.albums, lv.nal) : 0;
    }
    case LV_TRACKS: {
        struct lib_album a;
        return param < lv.nal && !lib_album(param, &a) ?
               range_count(hid, 0, a.first_track, a.tracks, lv.ntr) : 0;
    }
    case LV_YEAR_ALBUMS:
    case LV_GENRE_ALBUMS:
        return make_list(kind, param) ? 0 : lv.list_n;
    }
    return 0;
}

int lv_item(int kind, uint32_t param, uint32_t i, uint32_t *id)
{
    int32_t p = -1;
    if (!lv.ready)
        return -1;
    switch (kind) {
    case LV_ARTISTS:
        p = nth_set(rvis, rpre, lv.nar, i);
        break;
    case LV_ALBUMS:
        p = nth_set(avis, apre, lv.nal, i);
        break;
    case LV_GENRES:
        p = nth_set(gvis, gpre, lv.ngen, i);
        break;
    case LV_YEARS:
        p = i < lv.nyears ? years[i] : -1;
        break;
    case LV_SONGS: {
        int32_t s = nth_set(svis, spre, lv.ntr, i);
        uint32_t t;
        if (s >= 0 && !lib_song((uint32_t)s, &t))
            p = (int32_t)t;
        break;
    }
    case LV_ARTIST_ALBUMS: {
        struct lib_artist a;
        if (param < lv.nar && !lib_artist(param, &a))
            p = range_at(avis, 1, a.first_album, a.albums, lv.nal, i);
        break;
    }
    case LV_TRACKS: {
        struct lib_album a;
        if (param < lv.nal && !lib_album(param, &a))
            p = range_at(hid, 0, a.first_track, a.tracks, lv.ntr, i);
        break;
    }
    case LV_YEAR_ALBUMS:
    case LV_GENRE_ALBUMS:
        if (!make_list(kind, param) && i < lv.list_n)
            p = list[i];
        break;
    }
    if (p < 0)
        return -1;
    *id = (uint32_t)p;
    return 0;
}
