/* The library in the browser (ETUDE §127): its lists are folders of their own (fat_dir.vol
 * VOL_LIB, cluster: the list, size: its param), so that the browser's path, keys, rows and
 * scroll serve them too. Their rows come from libview.c, a few kept at a time; a list's folder
 * rows are F_DIR | F_LIB entries naming the list they open, its track rows the files
 * themselves. The index is opened (its CRC checked) at the first use, and again after an update
 * or a USB session; the card in or out sets the views up again. */
#include "ui_priv.h"
#include "board.h"
#include "emmc.h"
#include "fat.h"
#include "library.h"
#include "libview.h"

enum { LB_NONE, LB_OPEN, LB_FAILED };           /* the index: not opened yet, open, unusable */
static int state;
static uint32_t file_first;
static uint8_t lcache[8 * 520] __attribute__((section(".screenram"), aligned(4)));

static int io_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    return emmc_read(file_first + off / 512, n / 512, buf) ? -1 : 0;
}

static int card_in(void)
{
    uint32_t serial = lv_card_serial();
    return serial && sd_present() && !sd_open() && sdvol.serial == serial;
}

/* while the index is read: the watchdog, and the music (a queue of the file needs no index) */
static void bg(void)
{
    wdt_kick();
    ui_background();
}

/* The index opened and the views set up, if not yet: 0, or -1 (none, or unusable) */
static int ready(void)
{
    if (state == LB_OPEN && lv_ready())
        return 0;
    if (state == LB_FAILED)
        return -1;
    uint32_t t0 = ticks();
    if (state == LB_NONE) {
        struct fat_file f;
        uint32_t count;
        struct lib_io io = { io_read, 0 };
        state = LB_FAILED;
        if (fat_find_root(LIB_FILE, &f) || f.size != LIB_FILE_SIZE ||
            fat_extent(&f, &file_first, &count))
            return -1;
        lib_cache(lcache, sizeof lcache);
        lib_yield = bg;                         /* (the CRC: the whole half read) */
        int r = lib_open(&io);
        lib_yield = 0;
        if (r) {
            log_printf("library: no index to show (%d)", r);
            return -1;
        }
        state = LB_OPEN;
    }
    int c = card_in();
    lv_yield = bg;
    int r = lv_setup(c);
    lv_yield = 0;
    log_printf("library: views of %u tracks, the card's %s, in %u ms: %u shown", lib_count(LIB_TRACKS),
               lv_card_serial() ? c ? "in" : "out" : "none", (ticks() - t0) / 1000,
               lv_count(LV_SONGS, 0));
    if (r)
        state = LB_FAILED;
    return r;
}

void libv_close(void)
{
    if (q_is_lib())                             /* its tracks were found through the index */
        q_clear();
    lib_close();
    lv_reset();
    state = LB_NONE;
    if (depth && path[1].dir.vol == VOL_LIB) {  /* shown: back to the top level */
        depth = 0;
        dir_load(&path[0].dir);                 /* (no disk read) */
        b_dirty = 1;
    }
}

/* A resume in the library (resume.c): the lists' names taken from their parents' rows, and
 * the list shown loaded, at the browser's first use (the index read then, not at boot) */
static int restore_due;

void libv_restore(void)
{
    restore_due = 1;
    copy_name(path[1].name, T("Library"), sizeof path[1].name);
    dir_n = 0;
}

int libv_restore_now(void)
{
    if (!restore_due)
        return 0;
    restore_due = 0;
    for (int i = 2; i <= depth; i++) {
        int n = libv_load((int)path[i - 1].dir.cluster, path[i - 1].dir.size);
        path[i].name[0] = 0;
        if (path[i - 1].sel < n)
            copy_name(path[i].name, libv_name(path[i - 1].sel), sizeof path[i].name);
    }
    return 1;
}

void libv_card_changed(void)
{
    lv_reset();
    if (depth > 1 && path[1].dir.vol == VOL_LIB) {
        depth = 1;                              /* the lists change: from the library's top */
        path[1].sel = path[1].top = 0;
    }
}

/* ---- the rows of the list shown ---- */
static int cur_kind;
static uint32_t cur_param;
static int cur_discs;                           /* the album's tracks on several discs */

#define SLOTS 12
static struct {
    int i;                                      /* the row, -1: none */
    struct entry e;
    char name[100];
} slot[SLOTS] __attribute__((section(".screenram")));     /* (not the BSS: emptied at each load) */
static int slot_next;

static const struct {
    int kind;
    const char *name;
} top_rows[] = { { LV_ARTISTS, N_("Artists") }, { LV_ALBUMS, N_("Albums") },
                 { LV_YEARS, N_("Years") }, { LV_GENRES, N_("Genres") },
                 { LV_SONGS, N_("All songs") } };
#define TOP_ROWS (int)(sizeof top_rows / sizeof top_rows[0])

int libv_load(int kind, uint32_t param)
{
    cur_kind = kind;
    cur_param = param;
    for (int k = 0; k < SLOTS; k++)
        slot[k].i = -1;
    cur_discs = 0;
    if (ready())
        return 0;
    if (kind == 0)
        return TOP_ROWS;
    if (kind == LV_TRACKS) {
        struct lib_album a;
        struct lib_track x, y;
        cur_discs = !lib_album(param, &a) && a.tracks && !lib_track(a.first_track, &x) &&
                    !lib_track(a.first_track + a.tracks - 1, &y) && x.disc != y.disc;
    }
    return (int)lv_count(kind, param);
}

const char *libv_empty(void)
{
    return state == LB_OPEN ? T("nothing here") : T("No library yet: Settings, Library, PLAY");
}

static char *put_s(char *p, char *end, const char *s)
{
    while (*s && p < end)
        *p++ = *s++;
    return p;
}

/* "name - other" (other left out when empty) */
static void two(char *b, int max, const char *name, const char *other)
{
    char *p = put_s(b, b + max - 1, name);
    if (*other) {
        p = put_s(p, b + max - 1, " - ");
        p = put_s(p, b + max - 1, other);
    }
    *p = 0;
}

static void album_text(uint32_t al, int with_artist, char *b, int max)
{
    struct lib_album a;
    struct lib_artist r;
    char name[100], other[100];
    other[0] = 0;
    if (lib_album(al, &a) || lib_str(a.name, name, sizeof name) < 0)
        name[0] = 0;
    if (!name[0])
        copy_name(name, T("Unknown album"), sizeof name);
    if (with_artist) {
        if (lib_artist(a.artist, &r) || lib_str(r.name, other, sizeof other) < 0)
            other[0] = 0;
    } else if (a.year) {                        /* "name (year)" */
        char *p = log_format(other, other + sizeof other - 1, "%s (%u)", name, a.year);
        *p = 0;
        copy_name(name, other, sizeof name);
        other[0] = 0;
    }
    two(b, max, name, other);
}

/* A track as a queue's entry (and its file name), or 0 */
static int track_entry(uint32_t t, struct entry *e, struct lib_track *o)
{
    if (lib_track(t, o))
        return -1;
    e->cluster = o->cluster;
    e->size = o->size;
    e->name = 0;
    e->flags = (uint16_t)((o->codec & F_CODEC) | (o->vol == 1 ? F_SD : 0) |
                          (o->contig ? F_CONTIG : 0));
    return 0;
}

static void row(int i, struct entry *e, char *b, int max)
{
    uint32_t id = 0;
    b[0] = 0;
    e->cluster = e->size = 0;
    e->name = 0;
    e->flags = F_DIR | F_LIB;
    if (cur_kind == 0) {
        if (i < TOP_ROWS) {
            char *p = log_format(b, b + max - 1, "%s (%u)", tr(top_rows[i].name),
                                 lv_count(top_rows[i].kind, 0));
            *p = 0;
            e->cluster = (uint32_t)top_rows[i].kind;
        }
        return;
    }
    if (lv_item(cur_kind, cur_param, (uint32_t)i, &id))
        return;
    char s[100];
    switch (cur_kind) {
    case LV_ARTISTS: {
        struct lib_artist a;
        if (lib_artist(id, &a) || lib_str(a.name, b, (uint32_t)max) < 0)
            b[0] = 0;
        if (!b[0])
            copy_name(b, T("Unknown artist"), max);
        e->cluster = LV_ARTIST_ALBUMS;
        e->size = id;
        break;
    }
    case LV_GENRES: {
        struct lib_genre g;
        if (lib_genre(id, &g) || lib_str(g.name, b, (uint32_t)max) < 0)
            b[0] = 0;
        if (!b[0])
            copy_name(b, T("Unknown genre"), max);
        e->cluster = LV_GENRE_ALBUMS;
        e->size = id;
        break;
    }
    case LV_YEARS: {
        if (id) {
            char *p = log_format(b, b + max - 1, "%u", id);
            *p = 0;
        } else
            copy_name(b, T("Unknown year"), max);
        e->cluster = LV_YEAR_ALBUMS;
        e->size = id;
        break;
    }
    case LV_ARTIST_ALBUMS:
    case LV_ALBUMS:
    case LV_YEAR_ALBUMS:
    case LV_GENRE_ALBUMS:
        album_text(id, cur_kind != LV_ARTIST_ALBUMS, b, max);
        e->cluster = LV_TRACKS;
        e->size = id;
        break;
    case LV_TRACKS:
    case LV_SONGS: {
        struct lib_track t;
        if (track_entry(id, e, &t))
            return;
        if (lib_str(t.title, s, sizeof s) < 0 || !s[0])
            if (lib_str(t.file, s, sizeof s) < 0)
                s[0] = 0;
        if (cur_kind == LV_TRACKS) {
            char *p = b;
            if (t.track && cur_discs && t.disc)
                p = log_format(p, b + max - 1, "%u-%02u ", t.disc, t.track);
            else if (t.track)
                p = log_format(p, b + max - 1, "%02u ", t.track);
            p = put_s(p, b + max - 1, s);
            *p = 0;
        } else {
            char a[100];
            if (lib_str(t.artist, a, sizeof a) < 0)
                a[0] = 0;
            two(b, max, s, a);
        }
        break;
    }
    }
}

static int slot_of(int i)
{
    for (int k = 0; k < SLOTS; k++)
        if (slot[k].i == i)
            return k;
    int k = slot_next;
    slot_next = (slot_next + 1) % SLOTS;
    row(i, &slot[k].e, slot[k].name, sizeof slot[k].name);
    slot[k].i = i;
    return k;
}

const char *libv_name(int i)
{
    return slot[slot_of(i)].name;
}

const struct entry *libv_ent(int i)
{
    return &slot[slot_of(i)].e;
}

/* ---- the tracks of a list for the queue (queue.c, ETUDE §128) ---- */
static uint16_t *c_ids;
static uint32_t c_n;
static int c_max;

static void collect_album(uint32_t al)
{
    uint32_t m = lv_count(LV_TRACKS, al), id;
    for (uint32_t i = 0; i < m && c_n < (uint32_t)c_max; i++)
        if (!lv_item(LV_TRACKS, al, i, &id))
            c_ids[c_n++] = (uint16_t)id;
    if (c_n % 1024 < m)
        wdt_kick();
}

static void collect_albums(int kind, uint32_t param)
{
    uint32_t m = lv_count(kind, param), al;
    for (uint32_t i = 0; i < m; i++)
        if (!lv_item(kind, param, i, &al))
            collect_album(al);
}

uint32_t libv_collect(int kind, uint32_t param, uint16_t *ids, int max, int *on_sd)
{
    *on_sd = 0;
    if (ready())
        return 0;
    c_ids = ids;
    c_n = 0;
    c_max = max;
    uint32_t m, id;
    switch (kind) {
    case LV_TRACKS:
    case LV_SONGS:
        m = lv_count(kind, param);
        for (uint32_t i = 0; i < m && c_n < (uint32_t)max; i++) {
            if (!lv_item(kind, param, i, &id))
                ids[c_n++] = (uint16_t)id;
            if (i % 1024 == 0)
                wdt_kick();
        }
        break;
    case LV_ARTIST_ALBUMS:
    case LV_ALBUMS:
    case LV_YEAR_ALBUMS:
    case LV_GENRE_ALBUMS:
        collect_albums(kind, param);
        break;
    case LV_ARTISTS:                            /* every album, in the artists' order */
        collect_albums(LV_ALBUMS, 0);
        break;
    case LV_YEARS:
    case LV_GENRES:                             /* year by year, genre by genre */
        m = lv_count(kind, 0);
        for (uint32_t i = 0; i < m; i++)
            if (!lv_item(kind, 0, i, &id))
                collect_albums(kind == LV_YEARS ? LV_YEAR_ALBUMS : LV_GENRE_ALBUMS, id);
        break;
    }
    *on_sd = lv_card_serial() && card_in();        /* (the card's tracks shown: some may be) */
    return c_n;
}

int libv_track_id(uint32_t id, struct entry *e, char *name, int max)
{
    struct lib_track t;
    if (ready() || track_entry(id, e, &t))
        return -1;
    if (lib_str(t.file, name, (uint32_t)max) < 0)
        name[0] = 0;
    return 0;
}
