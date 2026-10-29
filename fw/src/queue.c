/* Play queue (ETUDE §25, §79): the files of a folder, or all those under one (PLAY held), and the
 * play modes. The queue is a table of folders (in PMU-SRAM, up to Q_DIRS), each with its number
 * of tracks: track k is found by these counts, and its folder read again into the queue's own
 * listing (in .queuebuf) when needed, in the browser's order (folders first, natural order).
 * The tracks used lately (the one heard, the one decoded, the browser's) keep their entry and
 * name in a few slots. Shuffle is a permutation of all the tracks, without a table: a Feistel
 * network on the next power of 4, cycle walking down to [0, q_n). */
#include "ui_priv.h"

int q_n, q_cur = -1, q_playing, q_errors;
int q_dec;                                      /* the track core B decodes; q_cur: the one heard */
char q_folder[64];

/* ---- the folders of the queue ---- */
struct q_dir {
    uint32_t cluster, size;
    uint16_t count;                             /* its tracks */
    uint8_t vol, flags;                         /* QD_CONTIG, its level (QD_LEVEL) */
};
#define QD_CONTIG 0x80
#define QD_LEVEL  0x0f
#define Q_LEVELS  8                             /* the folder held and 7 levels under it */
static struct q_dir qd[Q_DIRS] __attribute__((section(".pmusram")));
static int qd_n;

/* The queue's listing: one folder at a time */
#define QL_MAX 600
static struct {
    struct entry e[QL_MAX];
    uint16_t order[QL_MAX];
    char pool[0x8000 - QL_MAX * (sizeof(struct entry) + 2)];
} ql_buf __attribute__((section(".queuebuf")));
static struct listing ql = { ql_buf.e, ql_buf.order, ql_buf.pool, QL_MAX, sizeof ql_buf.pool,
                             0, 0, 0, 1, 1 };
static int ql_dir = -1;                         /* the qd[] in ql, -1: none */
static int ql_files;                            /* ql's first file in its order (folders first) */

static void qd_fat_dir(int f, struct fat_dir *d)
{
    d->cluster = qd[f].cluster;
    d->size = qd[f].size;
    d->vol = qd[f].vol;
    d->contig = (qd[f].flags & QD_CONTIG) != 0;
}

/* Folder f into ql: 0, or < 0 on a read error, or while ql is being read. A track's folder is
 * read quietly (ql.quiet: a few ms, from within the playback); the walk of q_from_tree keeps the
 * screen and the music going, and a track asked for meanwhile gets "" (not a read within it). */
static int ql_load(int f)
{
    static int busy;
    if (ql_dir == f)
        return 0;
    if (busy)
        return -1;
    struct fat_dir d;
    qd_fat_dir(f, &d);
    ql_dir = -1;
    busy = 1;
    int r = list_load(&ql, &d);
    busy = 0;
    if (r < 0)
        return -1;
    ql_files = 0;
    while (ql_files < ql.n && (ql.e[ql.order[ql_files]].flags & F_DIR))
        ql_files++;
    ql_dir = f;
    return 0;
}

/* ---- the tracks used lately: their entry and name ---- */
#define SLOTS 4
static struct {
    int i;                                      /* the track, -1: free */
    uint32_t used;                              /* for the least recently used */
    struct entry e;
    char name[NAME_KEEP + 1];
} slot[SLOTS];
static uint32_t slot_clock;

static void slots_clear(void)
{
    for (int s = 0; s < SLOTS; s++)
        slot[s].i = -1;
}

static int slot_of(int i)                       /* the slot of track i, read if needed; -1 */
{
    int s, old = 0;
    for (s = 0; s < SLOTS; s++)
        if (slot[s].i == i) {
            slot[s].used = ++slot_clock;
            return s;
        }
    if (i < 0 || i >= q_n)
        return -1;
    int f = 0, k = i;
    while (f < qd_n && k >= qd[f].count)
        k -= qd[f++].count;
    if (f >= qd_n || ql_load(f) || ql_files + k >= ql.n)
        return -1;                              /* not readable, or changed since */
    for (s = 1; s < SLOTS; s++)
        if (slot[s].used < slot[old].used)
            old = s;
    const struct entry *e = &ql.e[ql.order[ql_files + k]];
    slot[old].i = i;
    slot[old].used = ++slot_clock;
    slot[old].e = *e;
    copy_name(slot[old].name, ql.pool + e->name, sizeof slot[old].name);
    return old;
}

const struct entry *q_ent(int i)
{
    int s = slot_of(i);
    return s < 0 ? 0 : &slot[s].e;
}

const char *q_name(int i)
{
    int s = slot_of(i);
    return s < 0 ? "" : slot[s].name;
}

void q_set_codec(int i, uint32_t codec)         /* M4A: AAC or ALAC, from its content */
{
    int s = slot_of(i);
    if (s >= 0)
        slot[s].e.flags = (uint16_t)((slot[s].e.flags & ~F_CODEC) | codec);
}

int q_on_sd(void)
{
    return qd_n && qd[0].vol == FAT_VOL_SD;
}

void q_clear(void)
{
    q_n = qd_n = 0;
    q_cur = 0;
    ql_dir = -1;
    slots_clear();
}

/* ---- play modes (BACK held on the play screen): in order, repeat all, repeat one, shuffle ---- */
int play_mode;
uint32_t rnd = 0x2545f491;                      /* xorshift, stirred by the clock (playscreen.c) */
static uint32_t q_seed;

static uint32_t rnd_next(void)
{
    rnd ^= rnd << 13;
    rnd ^= rnd >> 17;
    rnd ^= rnd << 5;
    return rnd;
}

static uint32_t mix(uint32_t x, uint32_t k)
{
    x ^= k;
    x *= 0x9e3779b1u;
    x ^= x >> 15;
    x *= 0x85ebca77u;
    return x ^ x >> 13;
}

/* One pass of the Feistel network on 2h bits (4 rounds), or its inverse */
static uint32_t feistel(uint32_t x, uint32_t h, int inv)
{
    uint32_t m = (1u << h) - 1, l = x >> h, r = x & m;
    for (int k = 0; k < 4; k++) {
        if (!inv) {                             /* (l, r) -> (r, l ^ F(r)) */
            uint32_t t = l ^ (mix(r, q_seed + (uint32_t)k) & m);
            l = r;
            r = t;
        } else {                                /* (l, r) -> (r ^ F(l), l), keys reversed */
            uint32_t t = r ^ (mix(l, q_seed + 3u - (uint32_t)k) & m);
            r = l;
            l = t;
        }
    }
    return l << h | r;
}

/* Position p of the shuffled order -> track (inv: track -> position), in [0, q_n) */
static int perm(int p, int inv)
{
    uint32_t h = 1;
    while ((1u << (2 * h)) < (uint32_t)q_n)
        h++;
    uint32_t x = (uint32_t)p;
    do
        x = feistel(x, h, inv);
    while (x >= (uint32_t)q_n);                 /* cycle walking: at most a few passes */
    return (int)x;
}

void q_shuffle_new(void)                        /* another shuffled order */
{
    q_seed = rnd_next();
}

int q_next_index(int i, int user)        /* the track after i, or -1: end of the queue */
{
    if (!q_n)
        return -1;
    if (play_mode == MODE_REPEAT_ONE && !user)
        return i;
    if (play_mode == MODE_SHUFFLE && q_n > 1) {
        int p = perm(i, 1) + 1;
        if (p < q_n)
            return perm(p, 0);
        q_shuffle_new();                        /* all played: another order, not i first */
        int j = perm(0, 0);
        return j != i ? j : perm(1, 0);
    }
    if (i + 1 < q_n)
        return i + 1;
    return play_mode == MODE_ORDER ? -1 : 0;
}

int q_prev_index(int i)                  /* the track before i (BACK), or -1 */
{
    if (play_mode == MODE_SHUFFLE && q_n > 1) {
        int p = perm(i, 1);
        return p > 0 ? perm(p - 1, 0) : -1;
    }
    return i - 1;
}

/* ---- building the queue ---- */
static void q_done(const char *name, uint32_t t0)
{
    q_n = 0;
    for (int f = 0; f < qd_n; f++)
        q_n += qd[f].count;
    slots_clear();
    q_shuffle_new();
    copy_name(q_folder, name, sizeof q_folder);
    log_printf("ui: %d tracks in %d folders under %s, %u ms", q_n, qd_n, name,
               (ticks() - t0) / 1000);
}

/* qd[from..qd_n) to start at qd[to] (no memmove in this firmware) */
static void qd_move(int to, int from)
{
    int n = qd_n - from;
    if (to > from)
        for (int k = n - 1; k >= 0; k--)
            qd[to + k] = qd[from + k];
    else
        for (int k = 0; k < n; k++)
            qd[to + k] = qd[from + k];
}

static void qd_set(int f, const struct fat_dir *d, int level)
{
    qd[f].cluster = d->cluster;
    qd[f].size = d->size;
    qd[f].count = 0;
    qd[f].vol = d->vol;
    qd[f].flags = (uint8_t)((d->contig ? QD_CONTIG : 0) | level);
}

/* The playable files of the folder shown, from the one at 'start' (or the next playable) */
void q_from_dir(int start)
{
    uint32_t t0 = ticks();
    q_clear();
    qd_set(0, &path[depth].dir, 0);
    qd_n = 1;
    if (ql_load(0) == 0)
        qd[0].count = (uint16_t)(ql.n - ql_files);
    q_done(path[depth].name, t0);
    for (int i = start; i < dir_n; i++) {      /* the one chosen, or the next if it is empty */
        const struct entry *e = ent(i);
        if ((e->flags & F_DIR) || !e->size)
            continue;
        for (int k = ql_files; k < ql.n; k++)
            if (ql.e[ql.order[k]].cluster == e->cluster) {
                q_cur = k - ql_files;
                return;
            }
        break;
    }
}

/* PLAY held on a folder: everything under it, the files of each folder in order, then its
 * subfolders in order, depth first (Q_LEVELS levels, Q_DIRS folders with tracks). Each folder
 * listed is replaced in the table by its subfolders (inserted right after it, which gives the
 * depth-first order) and kept only when it has tracks. */
void q_from_tree(const struct fat_dir *top, const char *name)
{
    uint32_t t0 = ticks();
    int lost = 0;
    q_clear();
    qd_set(0, top, 0);
    qd_n = 1;
    ql.quiet = 0;
    for (int p = 0; p < qd_n;) {
        int level = qd[p].flags & QD_LEVEL;
        if (ql_load(p) < 0) {                   /* unreadable: left out */
            ql.n = ql_files = 0;
        }
        int subs = level + 1 < Q_LEVELS ? ql_files : 0;
        if (subs > Q_DIRS - qd_n) {
            lost += subs - (Q_DIRS - qd_n);
            subs = Q_DIRS - qd_n;
        }
        int files = ql.n - ql_files;
        struct fat_dir d;
        qd_move(p + 1 + subs, p + 1);
        for (int s = 0; s < subs; s++) {
            dir_of_entry(&ql.e[ql.order[s]], &d);
            qd_set(p + 1 + s, &d, level + 1);
        }
        qd_n += subs;
        ql_dir = -1;                            /* the indexes move */
        if (files) {
            qd[p].count = (uint16_t)files;
            p++;
        } else {                                /* no track: out of the table */
            qd_move(p, p + 1);
            qd_n--;
        }
    }
    ql.quiet = 1;
    if (lost)
        log_printf("ui: %d folders left out (%d at most)", lost, Q_DIRS);
    q_done(name, t0);
    q_cur = 0;
}
