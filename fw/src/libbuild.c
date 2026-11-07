/* The library's index built on the device (libbuild.h, docs/LIBRARY.md, ETUDE §125-§126).
 *
 * Nothing plays meanwhile: the work memory is core B's (HIGHRAM0 and HIGHRAM1, 384 KiB) and the
 * input ring in SYSRAM (96 KiB). The index goes into the half of NEWFWLIB.DAT not in use; the
 * header is rewritten last, so that a cut or a stop leaves the index before.
 *   1. count: the audio files of the volumes (directories only), for the progress;
 *   2. scan: each file's fields, from the index before when its volume, first cluster, size and
 *      last write are the same, else from its tags; the strings appended to STRS (at the start
 *      of the half: the names deduplicated through a table in RAM), a record of 40 bytes per
 *      track written from the end of the half down, 8 bytes per track kept in RAM;
 *   3. sort, in RAM: the album artists, the albums' names and the genres by their keys (the
 *      first 4 folded characters in RAM, the strings read when they are equal), then the tracks;
 *   4. write: TRKS in album order (the records read back), ALBS, ARTS, GENS, GALB, SONG (titles
 *      sorted the same way), CLUS, VOLS, the section table; the half's CRC read back; the header.
 * The order of the strings in STRS differs from mklib.py's (which needs all of them first):
 * the content is the same, every answer of the reader too (the emulator's tests compare them). */
#include <stdint.h>
#include "board.h"
#include "emmc.h"
#include "fat.h"
#include "hifi.h"
#include "libbuild.h"
#include "library.h"
#include "log.h"
#include "sd.h"
#include "tags.h"

#define NSEC       9
#define T0         (16u + 16u * NSEC)           /* STRS: after the section table */
#define HALF_SEC   8192u                        /* 4 MiB */
#define HALF       (HALF_SEC * 512u)
#define REC        40u
#define MAX_TRACKS 24576u
#define MAX_NAMES  8192u
#define MAX_DIRS   4096u
#define MAX_PAIRS  12288u
#define CACHE      64u

/* ---- the work memory ---- */
struct bt { uint16_t aa, album, track; uint8_t disc, pad; };     /* per track, in RAM */
struct bdir { uint32_t cluster, size; uint8_t vol, contig, pad[2]; };
#define HR 0x01000000u
static struct bt *const trk = (struct bt *)HR;                    /* 0x30000 */
static uint32_t *const noff = (uint32_t *)(HR + 0x30000);         /* names: STRS offsets */
static uint32_t *const nhash = (uint32_t *)(HR + 0x38000);        /* their hashes; then keys */
static uint16_t *const slots = (uint16_t *)(HR + 0x40000);        /* hash slots (id + 1) */
static uint8_t (*const cache)[512] = (uint8_t (*)[512])(HR + 0x48000);
static uint32_t *const ctag = (uint32_t *)(HR + 0x50000);         /* the sectors cached */
static uint16_t *const rank_a = (uint16_t *)(HR + 0x50100);       /* album artist: its index */
static uint16_t *const rank_p = (uint16_t *)(HR + 0x54100);       /* album name: its rank */
static uint16_t *const gidx = (uint16_t *)(HR + 0x58100);         /* genre name: its index */
static uint8_t *const written = (uint8_t *)(HR + 0x5c100);        /* sectors written: 1 KiB */
static uint8_t *const gmark = (uint8_t *)(HR + 0x5c500);          /* genre names used: 1 KiB */
static uint8_t (*const sbuf)[512] = (uint8_t (*)[512])(HR + 0x5c900);   /* 2 string buffers */
static uint8_t (*const wbuf)[512] = (uint8_t (*)[512])(HR + 0x5cd00);   /* 3 write streams */
static uint8_t *const mark = (uint8_t *)(HR + 0x5d300);           /* names used: 1 KiB */
static uint16_t *const order = (uint16_t *)0x03020000u;           /* SYSRAM: 48 KiB */
static struct bdir *const dirs = (struct bdir *)0x0302c000u;      /* 48 KiB */
static uint32_t *const pairs = (uint32_t *)0x0302c000u;           /* (dirs: after the scan) */
/* after the scan, nhash and slots (64 KiB) hold the names' keys, then the names by index */
static uint16_t (*const nkey)[4] = (uint16_t (*)[4])(HR + 0x38000);
static uint16_t *const art_name = (uint16_t *)(HR + 0x38000);
static uint16_t *const gen_name = (uint16_t *)(HR + 0x3c000);
static uint32_t *const art_tab = (uint32_t *)(HR + 0x40000);      /* first << 16 | count */
static uint32_t *const gen_tab = (uint32_t *)(HR + 0x54100);      /* (rank_p, after the sort) */
#define MAX_GENRES 2048u                                          /* gen_tab: 16 KiB */
/* during an update's scan, the index before: its old albums, genres and artists to the new
 * names' ids (a track of an album already seen reads no name again), the CLUS fence, and the
 * sectors cached (in order's SYSRAM, unused until the sort) */
#define MAP_ALBUMS 12288u
#define MAP_GENRES 2048u
#define MAP_ARTISTS 512u
#define FENCE      384u                         /* 24,576 tracks / 64 */
static uint32_t *const amap = (uint32_t *)(HR + 0x50100);         /* aid << 16 | alid: 48 KiB */
#define OLD_CACHE  0x9000u
static uint32_t *const fence = (uint32_t *)(0x03020000u + OLD_CACHE);             /* 1.5 KiB */
static uint16_t *const gmap = (uint16_t *)(0x03020000u + OLD_CACHE + 0x600);      /* 4 KiB */
static uint32_t (*const amemo)[2] = (uint32_t (*)[2])(0x03020000u + OLD_CACHE + 0x1600);
_Static_assert(OLD_CACHE + 0x1600 + 8 * MAP_ARTISTS <= 0xc000, "past order's 48 KiB");
_Static_assert(0x50100 + 4 * MAP_ALBUMS <= 0x5c100, "amap past rank_a, rank_p, gidx");

static struct {
    const struct lb_hooks *h;
    uint32_t file, hb;                          /* NEWFWLIB.DAT's first sector, the half's */
    int half, old;                              /* the half written, an index before (1) */
    uint32_t n, total, names, ndirs, strs, read, reused, t_prog;
    int err, phase;
} b;

/* The sort and the write: one progress each, only going forward, over stages of given weights
 * (per mille of the phase) */
static uint32_t st_base, st_span, st_last;
static int prog(uint32_t done, uint32_t total);

static void stage(int phase, uint32_t base, uint32_t span)
{
    if (phase != b.phase)
        st_last = 0;
    b.phase = phase;
    st_base = base;
    st_span = span;
}

static int sprog(uint32_t done, uint32_t total)
{
    uint32_t v = st_base + (total ? (uint32_t)((uint64_t)st_span * (done < total ? done : total) /
                                               total) : 0);
    if (v < st_last)
        v = st_last;
    st_last = v;
    return prog(v, 1000);
}

static int prog(uint32_t done, uint32_t total)
{
    wdt_kick();
    if (ticks() - b.t_prog < 100000)
        return 0;
    b.t_prog = ticks();
    if (b.h && b.h->progress && b.h->progress(b.phase, done, total)) {
        b.err = LB_ECANCEL;
        return 1;
    }
    return 0;
}

/* ---- the half written: sectors cached, three write streams ---- */
static struct ws { uint32_t sec; int on; } ws[3];
static uint32_t tph[5];                         /* ticks at the start of each phase, the end */
static struct io_count io_sd0, io_mmc0;         /* the reads counted at the start */
/* the eMMC reads by where they go: the index before (its CRC, then the scan), the half written,
 * in the count, scan, sort and write phases */
static uint32_t rd_old_crc, rd_old, rd_half, ph_c[5];
#define MARK(k) (tph[k] = ticks(), ph_c[k] = emmc_io.cmds)

static int rd_sec(uint32_t s, uint8_t **p)
{
    for (int w = 0; w < 3; w++)                 /* a sector not written yet: its stream's */
        if (ws[w].on && ws[w].sec == s) {
            *p = wbuf[w];
            return 0;
        }
    uint32_t c = s % CACHE;
    if (ctag[c] != s) {
        ctag[c] = 0xffffffff;
        rd_half++;
        if (emmc_read(b.hb + s, 1, cache[c]))
            return -1;
        ctag[c] = s;
    }
    *p = cache[c];
    return 0;
}

static int rd(uint32_t off, void *buf, uint32_t n)
{
    uint8_t *d = buf, *p;
    while (n) {
        uint32_t s = off / 512, o = off % 512, k = 512 - o < n ? 512 - o : n;
        if (rd_sec(s, &p))
            return -1;
        for (uint32_t i = 0; i < k; i++)
            d[i] = p[o + i];
        d += k;
        off += k;
        n -= k;
    }
    return 0;
}

static int flush(int w)
{
    if (!ws[w].on)
        return 0;
    uint32_t s = ws[w].sec;
    if (emmc_write_user(b.hb + s, 1, wbuf[w]))
        return -1;
    written[s / 8] |= (uint8_t)(1 << (s % 8));
    if (ctag[s % CACHE] == s)                   /* the cache follows */
        for (int i = 0; i < 512; i++)
            cache[s % CACHE][i] = wbuf[w][i];
    ws[w].on = 0;
    return 0;
}

/* n bytes at off of the half through stream w (a sector already written is read first) */
static int put(int w, uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *q = src;
    while (n) {
        uint32_t s = off / 512, o = off % 512, k = 512 - o < n ? 512 - o : n;
        if (!ws[w].on || ws[w].sec != s) {
            if (flush(w))
                return -1;
            for (int i = 0; i < 3; i++)         /* another stream on that sector: its bytes */
                if (i != w && ws[i].on && ws[i].sec == s && flush(i))
                    return -1;
            if (written[s / 8] & (1 << (s % 8))) {
                uint8_t *p;
                ctag[s % CACHE] = 0xffffffff;
                if (rd_sec(s, &p))
                    return -1;
                for (int i = 0; i < 512; i++)
                    wbuf[w][i] = p[i];
            } else
                for (int i = 0; i < 512; i++)
                    wbuf[w][i] = 0;
            ws[w].sec = s;
            ws[w].on = 1;
        }
        for (uint32_t i = 0; i < k; i++)
            wbuf[w][o + i] = q[i];
        q += k;
        off += k;
        n -= k;
    }
    return 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* a string of the half's STRS (ref) into sbuf[k] */
static const char *str_at(uint32_t ref, int k)
{
    uint32_t n = 0;
    while (n < 511) {
        uint8_t c;
        if (rd(T0 + ref + n, &c, 1) || !c)
            break;
        sbuf[k][n++] = c;
    }
    sbuf[k][n] = 0;
    return (const char *)sbuf[k];
}

static uint32_t str_add(const char *s)
{
    if (!*s)
        return 0;
    uint32_t ref = b.strs, n = 0;
    while (s[n])
        n++;
    if (put(0, T0 + b.strs, s, n + 1))
        b.err = LB_EIO;
    b.strs += n + 1;
    return ref;
}

static int same(const char *a, const char *c)
{
    while (*a && *a == *c)
        a++, c++;
    return *a == *c;
}

/* a name (artist, album, genre): its id, the same string always giving the same id */
static uint32_t name_id(const char *s)
{
    uint32_t h = 0x811c9dc5;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++)
        h = (h ^ *p) * 0x01000193;
    for (uint32_t k = h % (2 * MAX_NAMES);; k = (k + 1) % (2 * MAX_NAMES)) {
        uint32_t id = slots[k];
        if (!id) {
            if (b.names >= MAX_NAMES) {
                b.err = LB_ENAMES;
                return 0;
            }
            id = b.names++;
            noff[id] = str_add(s);
            nhash[id] = h;
            slots[k] = (uint16_t)(id + 1);
            return id;
        }
        id--;
        if (nhash[id] == h && same(str_at(noff[id], 0), s))
            return id;
    }
}

/* ---- the scan ---- */
/* the tags read, per codec, and the slowest files (where a build's time goes: the log) */
struct fs { uint32_t files, sectors, ms; };
static struct fst { struct fs by[10]; struct { uint32_t ms, sectors; char name[48]; } slow[3]; }
    *const fst = (struct fst *)(HR + 0x5f000);
static void tags_wait(void)                     /* before each read of tags_read() */
{
    prog(b.n, b.total);                         /* (BACK: taken after this file) */
}

static int old_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    rd_old++;
    return emmc_read(b.file + off / 512, n / 512, buf) ? -1 : 0;
}

/* the fields of the file scanned: in HIGHRAM too (not the BSS) */
static struct sf {
    struct tags f_tg;
    char f_title[96], f_artist[128], f_album[128], f_aa[128], f_genre[48];
} *const sf = (struct sf *)(HR + 0x5dc00);
_Static_assert(0x5dc00 + sizeof(struct sf) <= 0x60000, "the fields past the HIGHRAM");


static void copy(char *d, const char *s, int max)
{
    int i = 0;
    for (; i < max - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* A file unchanged since the index before: its title into sf, its names' ids from the maps (the
 * index before read for those not seen yet). 1, or 0 (changed, not there, or a read failed) */
static int reuse(const struct fat_dirent *e, uint32_t codec, uint32_t id[4], uint32_t *track,
                 uint32_t *year, uint32_t *disc)
{
    uint32_t t;
    struct lib_track o;
    if (!b.old || lib_find(e->vol, e->cluster, &t) || lib_track(t, &o) || o.vol != e->vol ||
        o.size != e->size || o.mtime != e->mtime || o.codec != codec ||
        lib_str(o.title, sf->f_title, sizeof sf->f_title) < 0)
        return 0;
    uint32_t a = o.album < MAP_ALBUMS ? amap[o.album] : 0xffffffffu;
    if (a == 0xffffffffu) {
        struct lib_album al;
        struct lib_artist ar;
        if (lib_album(o.album, &al) || lib_artist(al.artist, &ar) ||
            lib_str(al.name, sf->f_album, sizeof sf->f_album) < 0 ||
            lib_str(ar.name, sf->f_aa, sizeof sf->f_aa) < 0)
            return 0;
        a = name_id(sf->f_aa) << 16 | name_id(sf->f_album);
        if (o.album < MAP_ALBUMS)
            amap[o.album] = a;
    }
    uint32_t g = o.genre < MAP_GENRES ? gmap[o.genre] : 0xffffu;
    if (g == 0xffffu) {
        struct lib_genre gn;
        if (lib_genre(o.genre, &gn) || lib_str(gn.name, sf->f_genre, sizeof sf->f_genre) < 0)
            return 0;
        g = name_id(sf->f_genre);
        if (o.genre < MAP_GENRES)
            gmap[o.genre] = (uint16_t)g;
    }
    uint32_t *m = amemo[(o.artist * 2654435761u) >> 23 & (MAP_ARTISTS - 1)];
    if (m[0] != o.artist + 1) {                 /* (the track's artist: a string of STRS) */
        if (lib_str(o.artist, sf->f_artist, sizeof sf->f_artist) < 0)
            return 0;
        m[0] = o.artist + 1;
        m[1] = name_id(sf->f_artist);
    }
    id[0] = a >> 16;
    id[1] = a & 0xffff;
    id[2] = g;
    id[3] = m[1];
    *track = o.track;
    *year = o.year;
    *disc = o.disc;
    return !b.err;
}

/* the fields of a file from its tags */
static void fields(const struct fat_dirent *e, uint32_t codec, uint32_t *track, uint32_t *year,
                   uint32_t *disc)
{
    struct tags *const tgp = &sf->f_tg;
    char *const title = sf->f_title, *const artist = sf->f_artist, *const album = sf->f_album,
         *const aa = sf->f_aa, *const genre = sf->f_genre;
    struct fat_file f;
    fat_file_of(e, &f);
    uint32_t t0 = ticks(), s0 = sd_io.sectors + emmc_io.sectors;
    tags_read(&f, codec, tgp);
    uint32_t ms = (ticks() - t0) / 1000, secs = sd_io.sectors + emmc_io.sectors - s0;
    struct fs *c = &fst->by[codec < 10 ? codec : 0];
    c->files++;
    c->sectors += secs;
    c->ms += ms;
    int w = 0;                                  /* the slowest files */
    for (int k = 1; k < 3; k++)
        if (fst->slow[k].ms < fst->slow[w].ms)
            w = k;
    if (ms > fst->slow[w].ms) {
        fst->slow[w].ms = ms;
        fst->slow[w].sectors = secs;
        copy(fst->slow[w].name, e->name, sizeof fst->slow[w].name);
    }
    copy(title, tgp->title, sizeof sf->f_title);
    copy(artist, tgp->artist, sizeof sf->f_artist);
    copy(album, tgp->album, sizeof sf->f_album);
    copy(aa, tgp->albumartist[0] ? tgp->albumartist : tgp->artist, sizeof sf->f_aa);
    copy(genre, tgp->genre, sizeof sf->f_genre);
    *track = tgp->track;
    *year = tgp->year;
    *disc = tgp->disc > 255 ? 255 : tgp->disc;
}

/* The fingerprint of the files (docs/LIBRARY.md): two sums of a hash of each file's volume (its
 * serial), first cluster, size, last write and codec, in any order; in the header (+40, +44).
 * The same files: nothing to do (the update ends after the count) */
static uint32_t fp_a, fp_b;

static void fp_add(const struct fat_dirent *e, uint32_t codec)
{
    uint8_t k[18];
    k[0] = e->vol;
    put32(k + 1, e->vol == FAT_VOL_SD ? sdvol.serial : 0);
    put32(k + 5, e->cluster);
    put32(k + 9, e->size);
    put32(k + 13, e->mtime);
    k[17] = (uint8_t)codec;
    uint32_t h = 0x811c9dc5;
    for (int i = 0; i < 18; i++)
        h = (h ^ k[i]) * 0x01000193;
    fp_a += h;
    fp_b += (h << 13 | h >> 19) ^ (h * 0x9e3779b1u);
}

static int scan_cb(const struct fat_dirent *e, void *ctx)
{
    int count_only = *(int *)ctx;
    if (b.err)
        return 1;
    if (e->attr & FAT_ATTR_DIR) {
        if (b.ndirs >= MAX_DIRS) {
            b.err = LB_EDIRS;
            return 1;
        }
        struct fat_dir d;
        fat_dir_of(e, &d);
        dirs[b.ndirs++] = (struct bdir){ d.cluster, d.size, d.vol, d.contig, { 0, 0 } };
        return 0;
    }
    uint32_t codec = file_codec(e->name);
    if (!codec || !e->size)
        return 0;
    if (count_only) {
        b.total++;
        fp_add(e, codec);
        return prog(b.total, 0);
    }
    if (b.n >= MAX_TRACKS) {
        b.err = LB_ETRACKS;
        return 1;
    }
    const char *const title = sf->f_title, *const artist = sf->f_artist,
               *const album = sf->f_album, *const aa = sf->f_aa, *const genre = sf->f_genre;
    uint32_t track, year, disc, id[4];
    if (reuse(e, codec, id, &track, &year, &disc))
        b.reused++;
    else {
        if (b.err)
            return 1;
        fields(e, codec, &track, &year, &disc);
        b.read++;
        id[0] = name_id(aa);
        id[1] = name_id(album);
        id[2] = name_id(genre);
        id[3] = name_id(artist);
    }
    uint8_t r[REC] = { 0 };
    uint32_t aid = id[0], alid = id[1], gid = id[2];
    put32(r, e->cluster);
    put32(r + 4, e->size);
    put32(r + 8, e->mtime);
    put32(r + 12, str_add(title));
    put32(r + 16, str_add(e->name));
    put32(r + 20, noff[id[3]]);
    put16(r + 24, gid);
    put16(r + 26, track);
    put16(r + 28, year);
    r[30] = (uint8_t)disc;
    r[31] = (uint8_t)codec;
    r[32] = e->vol;
    r[33] = e->contig;
    put16(r + 34, aid);
    put16(r + 36, alid);
    gmark[gid / 8] |= (uint8_t)(1 << (gid % 8));
    trk[b.n] = (struct bt){ (uint16_t)aid, (uint16_t)alid, (uint16_t)track, (uint8_t)disc, 0 };
    if (T0 + b.strs + 4096 >= HALF - REC * (b.n + 1)) {
        b.err = LB_EFULL;
        return 1;
    }
    if (put(1, HALF - REC * (b.n + 1), r, REC))
        b.err = LB_EIO;
    b.n++;
    return b.err || prog(b.n, b.total);
}

static int walk(int count_only)
{
    b.ndirs = 0;
    for (int v = FAT_VOL_INT; v <= FAT_VOL_SD; v++) {
        if (v == FAT_VOL_SD && !sdvol.mounted)
            continue;
        struct fat_dir r;
        fat_root(v, &r);
        dirs[b.ndirs++] = (struct bdir){ r.cluster, r.size, r.vol, r.contig, { 0, 0 } };
    }
    for (uint32_t i = 0; i < b.ndirs && !b.err; i++) {
        struct fat_dir d = { dirs[i].cluster, dirs[i].size, dirs[i].vol, dirs[i].contig };
        fat_list(&d, scan_cb, &count_only);
    }
    return b.err;
}

/* ---- the sorts (heapsort of 16-bit indexes, no recursion) ---- */
static int (*cmp_fn)(uint32_t, uint32_t);

static void sift(uint16_t *a, uint32_t i, uint32_t n)
{
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= n)
            return;
        if (c + 1 < n && cmp_fn(a[c], a[c + 1]) < 0)
            c++;
        if (cmp_fn(a[i], a[c]) >= 0)
            return;
        uint16_t t = a[i];
        a[i] = a[c];
        a[c] = t;
        i = c;
    }
}

static void hsort(uint16_t *a, uint32_t n, int (*cmp)(uint32_t, uint32_t))
{
    cmp_fn = cmp;
    for (uint32_t i = n / 2; i-- > 0;) {
        sift(a, i, n);
        if (i % 64 == 0)
            sprog(n / 2 - i, n + n / 2);
    }
    for (uint32_t m = n; m > 1; m--) {
        uint16_t t = a[0];
        a[0] = a[m - 1];
        a[m - 1] = t;
        sift(a, 0, m - 1);
        if (m % 64 == 0)
            sprog(n / 2 + n - m, n + n / 2);
    }
}

static int key_cmp(const uint16_t *x, const uint16_t *y, int n)
{
    for (int i = 0; i < n; i++)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

static int artist_role;
static int cmp_name(uint32_t x, uint32_t y)
{
    int c = key_cmp(nkey[x], nkey[y], 4);
    return c ? c : lib_cmp(str_at(noff[x], 0), str_at(noff[y], 1), artist_role);
}

/* the names marked in mark (a bitmap of ids), sorted: rank[id] = its place; their count */
static uint32_t rank_names(const uint8_t *used, uint16_t *rank, int as_artist)
{
    uint32_t n = 0;
    for (uint32_t id = 0; id < b.names; id++) {
        rank[id] = 0xffff;
        if (used[id / 8] & (1 << (id % 8))) {
            order[n++] = (uint16_t)id;
            lib_key(str_at(noff[id], 0), as_artist, nkey[id], 4, 0);
        }
    }
    artist_role = as_artist;
    hsort(order, n, cmp_name);
    for (uint32_t i = 0; i < n; i++)
        rank[order[i]] = (uint16_t)i;
    return n;
}

static int rec_read(uint32_t i, uint8_t *r)
{
    return rd(HALF - REC * (i + 1), r, REC);
}

static int cmp_track(uint32_t x, uint32_t y)
{
    const struct bt *p = &trk[x], *q = &trk[y];
    if (rank_a[p->aa] != rank_a[q->aa])
        return rank_a[p->aa] < rank_a[q->aa] ? -1 : 1;
    if (rank_p[p->album] != rank_p[q->album])
        return rank_p[p->album] < rank_p[q->album] ? -1 : 1;
    if (p->disc != q->disc)
        return p->disc < q->disc ? -1 : 1;
    if (p->track != q->track)
        return p->track < q->track ? -1 : 1;
    uint8_t r[REC], s[REC];
    if (rec_read(x, r) || rec_read(y, s))
        return 0;
    const uint8_t *f = (const uint8_t *)str_at(get32(r + 16), 0),
                  *g = (const uint8_t *)str_at(get32(s + 16), 1);
    for (; *f == *g && *f; f++, g++)
        ;
    if (*f != *g)
        return *f < *g ? -1 : 1;
    if (r[32] != s[32])
        return r[32] < s[32] ? -1 : 1;
    return get32(r) < get32(s) ? -1 : get32(r) > get32(s);
}

static uint32_t trks;                           /* TRKS's offset in the half */
#define TKEY 6                                  /* SONG: 6 characters per track, 288 KiB */
static uint16_t (*const tkey)[TKEY] = (uint16_t (*)[TKEY])HR;
static uint32_t *const ckey = (uint32_t *)HR;                     /* CLUS: per track */
/* the titles last read (equal keys: the sort compares one element to many) */
#define TCACHE 8
static struct tc { uint32_t i, age; char s[128]; } *const tc = (struct tc *)(HR + 0x5d700);
static uint32_t tc_age;

static const char *song_title(uint32_t i, int avoid)
{
    int c = 0;
    for (int k = 0; k < TCACHE; k++) {
        if (tc[k].i == i) {
            tc[k].age = ++tc_age;
            return tc[k].s;
        }
        if (k != avoid && (c == avoid || tc[k].age < tc[c].age))
            c = k;
    }
    uint8_t t[8];
    const char *s = "";
    if (!rd(trks + 36 * i + 12, t, 8)) {
        s = str_at(get32(t), 0);
        if (!*s)
            s = str_at(get32(t + 4), 0);
    }
    copy(tc[c].s, s, sizeof tc[c].s);
    tc[c].i = i;
    tc[c].age = ++tc_age;
    return tc[c].s;
}

static int tc_slot(const char *s)
{
    return (int)(((const uint8_t *)s - (const uint8_t *)tc) / sizeof *tc);
}

static int cmp_song(uint32_t x, uint32_t y)    /* the keys only (then refined) */
{
    int c = key_cmp(tkey[x], tkey[y], TKEY);
    return c ? c : x < y ? -1 : x > y;
}

static int cmp_bytes(uint32_t x, uint32_t y)   /* folded titles equal: their bytes */
{
    const uint8_t *a = (const uint8_t *)song_title(x, -1);
    const uint8_t *c = (const uint8_t *)song_title(y, tc_slot((const char *)a));
    for (; *a == *c && *a; a++, c++)
        ;
    return *a != *c ? (*a < *c ? -1 : 1) : x < y ? -1 : x > y;
}

/* SONG: sorted by the first TKEY characters in RAM; then each run still equal gets the next
 * TKEY characters of its members (each title read once a round) and is sorted again, until
 * none is; titles equal once folded, by their bytes. tie[k]: order[k - 1] and order[k] equal
 * so far (1), equal to their ends (2) */
static uint8_t *const tie = (uint8_t *)(HR + 0x50100);           /* (ranks: free by now) */

static void ties_of(uint32_t s, uint32_t e)
{
    for (uint32_t k = s + 1; k < e; k++) {
        const uint16_t *x = tkey[order[k - 1]], *y = tkey[order[k]];
        tie[k] = 0;
        if (!key_cmp(x, y, TKEY)) {
            tie[k] = 1;
            for (int i = 0; i < TKEY; i++)
                if (!y[i])
                    tie[k] = 2;
        }
    }
}

static int song_sort(uint32_t n)
{
    hsort(order, n, cmp_song);
    tie[0] = 0;
    ties_of(0, n);
    for (int depth = 1, more = 1; more && depth < 24 && !b.err; depth++) {
        more = 0;
        for (uint32_t s = 0; s < n;) {
            uint32_t e = s + 1;
            while (e < n && tie[e] == 1)
                e++;
            if (e - s > 1) {
                for (uint32_t k = s; k < e; k++)
                    lib_key(song_title(order[k], -1), 0, tkey[order[k]], TKEY, depth * TKEY);
                hsort(order + s, e - s, cmp_song);
                ties_of(s, e);
                more = 1;
                if (sprog(s, n))
                    return b.err;
            }
            s = e;
        }
    }
    for (uint32_t s = 0; s < n;) {              /* equal once folded: by their bytes */
        uint32_t e = s + 1;
        while (e < n && tie[e])
            e++;
        if (e - s > 1)
            hsort(order + s, e - s, cmp_bytes);
        s = e;
    }
    return b.err;
}

static int cmp_clus(uint32_t x, uint32_t y)
{
    return ckey[x] != ckey[y] ? (ckey[x] < ckey[y] ? -1 : 1) : x < y ? -1 : x > y;
}

static int cmp_pair(uint32_t x, uint32_t y)    /* (pairs, through order) */
{
    return pairs[x] < pairs[y] ? -1 : pairs[x] > pairs[y];
}

/* ---- writing the index ---- */
static int write_index(struct lb_result *res)
{
    uint32_t n = b.n;
    lib_cache(0, 0);                            /* (order: ours from now on) */
    MARK(LB_SORT);
    if (flush(0) || flush(1) || flush(2))
        return LB_EIO;
    stage(LB_SORT, 0, 50);
    uint32_t ngen = rank_names(gmark, gidx, 0);         /* genres: their index */
    if (ngen > MAX_GENRES)
        return LB_ENAMES;
    for (uint32_t i = 0; i < 1024; i++)                 /* the album artists */
        mark[i] = 0;
    for (uint32_t i = 0; i < n; i++)
        mark[trk[i].aa / 8] |= (uint8_t)(1 << (trk[i].aa % 8));
    stage(LB_SORT, 50, 100);
    uint32_t nart = rank_names(mark, rank_a, 1);
    for (uint32_t i = 0; i < 1024; i++)                 /* the albums' names */
        mark[i] = 0;
    for (uint32_t i = 0; i < n; i++)
        mark[trk[i].album / 8] |= (uint8_t)(1 << (trk[i].album % 8));
    stage(LB_SORT, 150, 150);
    rank_names(mark, rank_p, 0);
    stage(LB_SORT, 300, 700);
    for (uint32_t i = 0; i < n; i++)
        order[i] = (uint16_t)i;
    hsort(order, n, cmp_track);
    if (b.err)
        return b.err;
    for (uint32_t id = 0; id < b.names; id++) {         /* index to name, for ARTS and GENS */
        if (rank_a[id] != 0xffff)
            art_name[rank_a[id]] = (uint16_t)id;
        if (gidx[id] != 0xffff)
            gen_name[gidx[id]] = (uint16_t)id;
    }
    /* TRKS, ALBS (written beside it), the artists' and the pairs' tables */
    MARK(LB_WRITE);
    stage(LB_WRITE, 0, 350);
    trks = (T0 + b.strs + 3) & ~3u;
    uint32_t albs = trks + 36 * n;
    if (albs + 16 * n >= HALF - REC * n)
        return LB_EFULL;
    uint32_t nalb = 0, first = 0, year = 0, npairs = 0, gseen[16], ng = 0;
    for (uint32_t i = 0; i < nart; i++)
        art_tab[i] = 0;
    for (uint32_t k = 0; k <= n; k++) {
        uint32_t i = k < n ? order[k] : 0, j = k ? order[k - 1] : 0;
        int newalb = k == n || !k || trk[i].aa != trk[j].aa || trk[i].album != trk[j].album;
        if (newalb && k) {                              /* the album before: closed */
            uint8_t a[16] = { 0 };
            put32(a, noff[trk[j].album]);
            put32(a + 4, first);
            put16(a + 8, k - first);
            put16(a + 10, rank_a[trk[j].aa]);
            put16(a + 12, year);
            if (put(2, albs + 16 * nalb, a, 16))
                return LB_EIO;
            uint32_t ar = rank_a[trk[j].aa];
            if (!(art_tab[ar] & 0xffff))
                art_tab[ar] = nalb << 16;
            art_tab[ar]++;
            for (uint32_t g = 0; g < ng; g++) {
                if (npairs >= MAX_PAIRS)
                    return LB_ENAMES;
                pairs[npairs++] = gseen[g] << 16 | nalb;
            }
            nalb++;
            if (nalb > 0xffff)
                return LB_ENAMES;
        }
        if (k == n)
            break;
        if (newalb) {
            first = k;
            year = 0;
            ng = 0;
        }
        uint8_t r[REC], t[36];
        if (rec_read(i, r))
            return LB_EIO;
        uint32_t gi = gidx[get16(r + 24)], y = get16(r + 28);
        if (y && (!year || y < year))
            year = y;
        uint32_t g = 0;
        while (g < ng && gseen[g] != gi)
            g++;
        if (g == ng) {
            if (ng == 16) {                     /* full: into the pairs (the sort removes */
                for (uint32_t q = 0; q < ng; q++) {     /* the duplicates) */
                    if (npairs >= MAX_PAIRS)
                        return LB_ENAMES;
                    pairs[npairs++] = gseen[q] << 16 | nalb;
                }
                ng = 0;
            }
            gseen[ng++] = gi;
        }
        for (int q = 0; q < 24; q++)                    /* cluster, size, mtime, 3 strings */
            t[q] = r[q];
        put16(t + 24, nalb);
        put16(t + 26, gi);
        put16(t + 28, get16(r + 26));
        put16(t + 30, y);
        t[32] = r[30];
        t[33] = r[31];
        t[34] = r[32];
        t[35] = r[33] & 1;
        if (put(1, trks + 36 * k, t, 36))
            return LB_EIO;
        if (sprog(k, n))
            return b.err;
    }
    uint32_t arts = albs + 16 * nalb, gens = arts + 8 * nart, galb = gens + 12 * ngen;
    for (uint32_t i = 0; i < nart; i++) {
        uint8_t a[8];
        put32(a, noff[art_name[i]]);
        put16(a + 4, art_tab[i] >> 16);
        put16(a + 6, art_tab[i] & 0xffff);
        if (put(2, arts + 8 * i, a, 8))
            return LB_EIO;
    }
    stage(LB_WRITE, 350, 30);
    for (uint32_t i = 0; i < npairs; i++)               /* GALB: by genre, then album */
        order[i] = (uint16_t)i;
    hsort(order, npairs, cmp_pair);
    if (b.err)
        return b.err;
    for (uint32_t g = 0; g < ngen; g++)
        gen_tab[2 * g] = gen_tab[2 * g + 1] = 0;
    uint32_t ngalb = 0;
    for (uint32_t k = 0; k < npairs; k++) {
        uint32_t p = pairs[order[k]];
        if (k && p == pairs[order[k - 1]])
            continue;
        if (!gen_tab[2 * (p >> 16) + 1])
            gen_tab[2 * (p >> 16)] = ngalb;
        gen_tab[2 * (p >> 16) + 1]++;
        uint8_t a[2];
        put16(a, p & 0xffff);
        if (put(2, galb + 2 * ngalb++, a, 2))
            return LB_EIO;
    }
    for (uint32_t g = 0; g < ngen; g++) {
        uint8_t a[12];
        put32(a, noff[gen_name[g]]);
        put32(a + 4, gen_tab[2 * g]);
        put32(a + 8, gen_tab[2 * g + 1]);
        if (put(1, gens + 12 * g, a, 12))
            return LB_EIO;
    }
    uint32_t song = (galb + 2 * ngalb + 3) & ~3u, clus = song + 4 * n, vols = clus + 8 * n;
    if (flush(0) || flush(1) || flush(2))
        return LB_EIO;
    /* SONG */
    stage(LB_WRITE, 380, 200);
    for (int k = 0; k < TCACHE; k++)
        tc[k].i = 0xffffffff;
    for (uint32_t i = 0; i < n; i++) {
        lib_key(song_title(i, -1), 0, tkey[i], TKEY, 0);
        order[i] = (uint16_t)i;
        if (sprog(i, n))
            return b.err;
    }
    stage(LB_WRITE, 580, 250);
    if (song_sort(n))
        return b.err;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t a[4];
        put32(a, order[k]);
        if (put(0, song + 4 * k, a, 4))
            return LB_EIO;
    }
    /* CLUS */
    stage(LB_WRITE, 830, 60);                   /* (each stage moves the bar: 0.102b4 stood here) */
    for (uint32_t i = 0; i < n; i++) {
        uint8_t t[36];
        if (rd(trks + 36 * i, t, 36))
            return LB_EIO;
        ckey[i] = (uint32_t)t[34] << 31 | get32(t);
        order[i] = (uint16_t)i;
        if (i % 256 == 0 && sprog(i, n))
            return b.err;
    }
    stage(LB_WRITE, 890, 40);
    hsort(order, n, cmp_clus);
    if (b.err)
        return b.err;
    stage(LB_WRITE, 930, 20);
    for (uint32_t k = 0; k < n; k++) {
        if (k % 256 == 0 && sprog(k, n))
            return b.err;
        uint8_t a[8];
        put32(a, ckey[order[k]]);
        put32(a + 4, order[k]);
        if (put(0, clus + 8 * k, a, 8))
            return LB_EIO;
    }
    /* VOLS */
    uint32_t nvol = 0, cnt[2] = { 0, 0 };
    for (uint32_t i = 0; i < n; i++)
        cnt[ckey[i] >> 31]++;
    for (uint32_t v = 0; v < 2; v++) {
        if (v && !sdvol.mounted && !cnt[1])
            continue;
        uint8_t a[12] = { (uint8_t)v };
        put32(a + 4, v ? sdvol.serial : 0);
        put32(a + 8, cnt[v]);
        if (put(0, vols + 12 * nvol++, a, 12))
            return LB_EIO;
    }
    uint32_t len = vols + 12 * nvol;
    /* the section table */
    static const char ids[] = "VOLSSTRSTRKSARTSALBSGENSGALBSONGCLUS";
    const uint32_t off[NSEC] = { vols, T0, trks, arts, albs, gens, galb, song, clus },
                   sz[NSEC] = { 12 * nvol, b.strs, 36 * n, 8 * nart, 16 * nalb, 12 * ngen,
                                2 * ngalb, 4 * n, 8 * n },
                   cn[NSEC] = { nvol, b.names, n, nart, nalb, ngen, ngalb, n, n };
    uint8_t h[T0];
    __builtin_memcpy(h, "NWLI", 4);
    put32(h + 4, 1);
    put32(h + 8, NSEC);
    put32(h + 12, len);
    for (int k = 0; k < NSEC; k++) {
        __builtin_memcpy(h + 16 + 16 * k, ids + 4 * k, 4);
        put32(h + 20 + 16 * k, off[k]);
        put32(h + 24 + 16 * k, sz[k]);
        put32(h + 28 + 16 * k, cn[k]);
    }
    if (put(1, 0, h, T0) || flush(0) || flush(1) || flush(2))
        return LB_EIO;
    /* the CRC, read back from the eMMC; then the header */
    stage(LB_WRITE, 950, 50);
    uint32_t crc = 0xffffffffu;
    for (uint32_t at = 0; at < len; at += 512) {
        if (emmc_read(b.hb + at / 512, 1, cache[0]))
            return LB_EIO;
        crc = lib_crc(crc, cache[0], len - at < 512 ? len - at : 512);
        if (at % (64 * 512) == 0)
            sprog(at, len);
    }
    ctag[0] = 0xffffffff;
    uint8_t *hd = cache[1];
    ctag[1] = 0xffffffff;
    if (emmc_read(b.file, 1, hd))
        return LB_EIO;
    uint32_t gen = __builtin_memcmp(hd, "NEWFWLIB", 8) ? 0 : get32(hd + 16);
    if (__builtin_memcmp(hd, "NEWFWLIB", 8))
        lib_empty_header(hd);
    put32(hd + 12, b.half);
    put32(hd + 16, gen + 1);
    put32(hd + 36, n);                          /* the tracks of the half in use (the status) */
    put32(hd + 40, fp_a);                       /* the files' fingerprint */
    put32(hd + 44, fp_b);
    put32(hd + 20 + 8 * b.half, len);
    put32(hd + 24 + 8 * b.half, ~crc);
    put32(hd + 508, ~lib_crc(0xffffffffu, hd, 508));
    if (emmc_write_user(b.file, 1, hd))
        return LB_EIO;
    res->artists = nart;
    res->albums = nalb;
    res->genres = ngen;
    res->bytes = len;
    return 0;
}

int lib_build(const struct lb_hooks *h, struct lb_result *res)
{
    uint32_t t0 = ticks();
    struct fat_file lf;
    int r = lib_file(&lf);
    if (r)
        return LB_EFILE;
    uint32_t first, count;
    if (fat_extent(&lf, &first, &count))
        return LB_EFILE;
    for (uint32_t i = 0; i < sizeof b; i++)
        ((uint8_t *)&b)[i] = 0;
    b.h = h;
    b.file = first;
    uint8_t *hd = (uint8_t *)dirs;              /* (not yet in use) */
    if (emmc_read(first, 1, hd))
        return LB_EIO;
    uint32_t act = get32(hd + 12), old_n = get32(hd + 36), old_a = get32(hd + 40),
             old_b = get32(hd + 44);
    b.half = act == 0 ? 1 : 0;
    b.hb = first + 1 + HALF_SEC * (uint32_t)b.half;
    struct lib_io io = { old_read, 0 };
    rd_old = 0;
    b.old = act <= 1 && !lib_open(&io);
    hifi_prepare();                             /* PD_HIGH on, core B held: its RAM is ours */
    hifi_own_drop();
    for (uint32_t i = 0; i < CACHE; i++)
        ctag[i] = 0xffffffff;
    for (uint32_t i = 0; i < 1024; i++)
        written[i] = gmark[i] = 0;
    for (uint32_t i = 0; i < 2 * MAX_NAMES; i++)
        slots[i] = 0;
    for (int i = 0; i < 3; i++)
        ws[i].on = 0;
    void (*yield)(void) = tags_yield;
    tags_yield = tags_wait;                     /* a file's tags may take seconds (a big cover
                                                 * on the card): the watchdog and the screen */
    lib_yield = wdt_kick;                       /* (the index before's CRC: up to 4 MiB read) */
    if (emmc_user_area(first, count)) {
        tags_yield = yield;
        return LB_EIO;
    }
    log_printf("library: building into half %c (%s)", 'A' + b.half,
               b.old ? "the index before reused" : "from the tags");
    b.phase = LB_COUNT;
    io_sd0 = sd_io;
    io_mmc0 = emmc_io;
    for (uint32_t i = 0; i < sizeof *fst; i++)
        ((uint8_t *)fst)[i] = 0;
    tags_fields_only = 1;
    MARK(LB_COUNT);
    tph[LB_SORT] = tph[LB_WRITE] = 0;
    ph_c[LB_SCAN] = ph_c[LB_SORT] = ph_c[LB_WRITE] = 0;
    rd_old_crc = rd_old;
    rd_half = 0;
    if (b.old) {                                /* the index before: sectors cached and maps */
        lib_cache(order, OLD_CACHE);            /* in order's SYSRAM, unused until the sort */
        for (uint32_t i = 0; i < MAP_ALBUMS; i++)
            amap[i] = 0xffffffffu;
        for (uint32_t i = 0; i < MAP_GENRES; i++)
            gmap[i] = 0xffff;
        for (uint32_t i = 0; i < MAP_ARTISTS; i++)
            amemo[i][0] = 0;
        if (lib_fence(fence, FENCE))
            log_printf("library: no CLUS fence (searched whole)");
    }
    fp_a = fp_b = 0;
    if (!walk(1) && b.old && b.total == old_n && fp_a == old_a && fp_b == old_b &&
        (fp_a || fp_b)) {
        log_printf("library: no change (%u tracks, the same fingerprint)", b.total);
        b.n = b.reused = b.total;               /* (nothing written: the index in use stays) */
        res->artists = lib_count(LIB_ARTISTS);
        res->albums = lib_count(LIB_ALBUMS);
        res->genres = lib_count(LIB_GENRES);
        res->bytes = 0;
    } else if (!b.err) {
        b.phase = LB_SCAN;
        MARK(LB_SCAN);
        uint8_t z = 0;
        if (put(0, T0, &z, 1))                  /* STRS: offset 0, the empty string */
            b.err = LB_EIO;
        b.strs = 1;
        if (!b.err && !walk(0))
            b.err = write_index(res);
    }
    r = b.err;
    MARK(4);
    tags_fields_only = 0;
    lib_cache(0, 0);
    emmc_user_area(0, 0);                       /* the fence closed again */
    tags_yield = yield;
    lib_yield = 0;
    lib_close();
    res->tracks = b.n;
    res->read = b.read;
    res->reused = b.reused;
    res->ms = (ticks() - t0) / 1000;
    if (tph[LB_SORT])
        log_printf("library: count %u ms, scan %u ms, sort %u ms, write %u ms",
                   (tph[LB_SCAN] - tph[LB_COUNT]) / 1000, (tph[LB_SORT] - tph[LB_SCAN]) / 1000,
                   ((tph[LB_WRITE] ? tph[LB_WRITE] : tph[4]) - tph[LB_SORT]) / 1000,
                   tph[LB_WRITE] ? (tph[4] - tph[LB_WRITE]) / 1000 : 0);
    log_printf("library: eMMC reads: the index before %u (its CRC %u), the half written %u; "
               "by phase %u, %u, %u, %u", rd_old, rd_old_crc, rd_half,
               (ph_c[LB_SCAN] ? ph_c[LB_SCAN] : ph_c[4]) - ph_c[LB_COUNT],
               ph_c[LB_SCAN] ? (ph_c[LB_SORT] ? ph_c[LB_SORT] : ph_c[4]) - ph_c[LB_SCAN] : 0,
               ph_c[LB_SORT] ? ph_c[LB_WRITE] - ph_c[LB_SORT] : 0,
               ph_c[LB_WRITE] ? ph_c[4] - ph_c[LB_WRITE] : 0);
    log_printf("library: reads: card %u commands, %u sectors, %u ms; eMMC %u commands, %u "
               "sectors, %u ms", sd_io.cmds - io_sd0.cmds, sd_io.sectors - io_sd0.sectors,
               (sd_io.us - io_sd0.us) / 1000, emmc_io.cmds - io_mmc0.cmds,
               emmc_io.sectors - io_mmc0.sectors, (emmc_io.us - io_mmc0.us) / 1000);
    static const char *const cn[10] = { "?", "WAV", "FLAC", "MP3", "Vorbis", "Opus", "AIFF",
                                        "M4A", "ALAC", "MPC" };
    for (int k = 0; k < 10; k++)
        if (fst->by[k].files)
            log_printf("library: tags of %u %s files: %u sectors, %u ms (%u KiB, %u ms a file)",
                       fst->by[k].files, cn[k], fst->by[k].sectors, fst->by[k].ms,
                       fst->by[k].sectors / 2 / fst->by[k].files, fst->by[k].ms / fst->by[k].files);
    for (int k = 0; k < 3; k++)
        if (fst->slow[k].ms)
            log_printf("library: slow: %s, %u ms, %u sectors", fst->slow[k].name, fst->slow[k].ms,
                       fst->slow[k].sectors);
    if (r)
        log_printf("library: not built (%d), the index before kept", r);
    else
        log_printf("library: %u tracks (%u read, %u from the index before), %u artists, "
                   "%u albums, %u genres, %u bytes, in %u ms", b.n, b.read, b.reused,
                   res->artists, res->albums, res->genres, res->bytes, res->ms);
    return r;
}
