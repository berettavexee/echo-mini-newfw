/* Play queue (ETUDE §25, §79): the files of a folder, or all those under one (PLAY held), and the
 * play modes. The queue is a table of folders (in PMU-SRAM, up to Q_DIRS), each with its number
 * of tracks: track k is found by these counts, and its folder read again into the queue's own
 * listing (in .screenram) when needed, in the browser's order (folders first, natural order).
 * The tracks used lately (the one heard, the one decoded, the browser's) keep their entry and
 * name in a few slots. Shuffle is a permutation of all the tracks, without a table: a Feistel
 * network on the next power of 4, cycle walking down to [0, q_n). */
#include "ui_priv.h"
#include "qfile.h"
#include "storage.h"

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
} ql_buf __attribute__((section(".screenram")));      /* (not loaded, not cleared) */
static struct listing ql = { ql_buf.e, ql_buf.order, ql_buf.pool, QL_MAX, sizeof ql_buf.pool,
                             0, 0, 0, 1, 1 };
static int ql_dir = -1;                         /* the qd[] in ql, -1: none */
/* Where the tracks come from (ETUDE §128): folders (qd, read again into ql), a library list
 * (the index's track numbers in ids, over qd: it is not used then), or NEWFWQ.DAT (qfile.c).
 * A library list is written into the file by the storage task while it plays; once written, the
 * queue is the file's (the same order). Tracks added or put next make it the file's first. */
enum { QM_DIRS, QM_IDS, QM_FILE };
static int q_mode;
static int q_sd;                                /* QM_IDS, QM_FILE: tracks on the card */
static uint16_t *const ids = (uint16_t *)(void *)qd;
#define IDS_MAX (int)(sizeof(qd) / (sizeof(uint16_t)))
static struct { int state; uint32_t token; } qw; /* the file being written (the storage task) */
enum { QW_NONE, QW_RUNNING, QW_DONE, QW_FAILED };
static uint32_t q_token;                        /* one more at each queue made */
static int un_anchor, un_at, un_n;              /* put next in shuffle: q_next_index() */
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
    if (q_mode == QM_IDS && qw.state == QW_DONE && qw.token == q_token)
        q_mode = QM_FILE;                       /* written: the file's from now on */
    if (q_mode != QM_DIRS) {                    /* a library list, or the file */
        for (s = 1; s < SLOTS; s++)
            if (slot[s].used < slot[old].used)
                old = s;
        int r = q_mode == QM_FILE ?
                qf_get((uint32_t)i, &slot[old].e, slot[old].name, sizeof slot[old].name) :
                libv_track_id(ids[i], &slot[old].e, slot[old].name, sizeof slot[old].name);
        if (r)
            return -1;
        slot[old].i = i;
        slot[old].used = ++slot_clock;
        return old;
    }
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
    return q_mode != QM_DIRS ? q_sd : qd_n && qd[0].vol == FAT_VOL_SD;
}

int q_is_lib(void)
{
    return q_mode == QM_IDS;
}

int q_is_file(void)
{
    if (q_mode == QM_IDS && qw.state == QW_DONE && qw.token == q_token)
        q_mode = QM_FILE;
    return q_mode == QM_FILE;
}

void q_clear(void)
{
    q_mode = QM_DIRS;
    q_token++;
    un_n = 0;
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

/* The order is the permutation turned by q_base: track 'from' (the one heard when the shuffle
 * starts, -1: none) first, then all the others before any comes again */
static int q_base;

static int shuf_pos(int i)                      /* track -> its place in the shuffled order */
{
    return (perm(i, 1) - q_base + q_n) % q_n;
}

static int shuf_at(int p)                       /* place -> track */
{
    return perm((p + q_base) % q_n, 0);
}

void q_shuffle_new(int from)                    /* another shuffled order, from track 'from' */
{
    q_seed = rnd_next();
    q_base = from >= 0 && from < q_n ? perm(from, 1) : 0;
}

/* Tracks put next in shuffle (q_add): the range [un_at, un_at + un_n) right after un_anchor,
 * then the shuffled order goes on from un_anchor (declared above) */

int q_next_index(int i, int user)        /* the track after i, or -1: end of the queue */
{
    if (!q_n)
        return -1;
    if (play_mode == MODE_REPEAT_ONE && !user)
        return i;
    if (play_mode == MODE_SHUFFLE && un_n > 0) {
        if (i == un_anchor)
            return un_at;
        if (i >= un_at && i < un_at + un_n - 1)
            return i + 1;
        if (i == un_at + un_n - 1)
            i = un_anchor;
    }
    if (play_mode == MODE_SHUFFLE && q_n > 1) {
        int p = shuf_pos(i) + 1;
        if (p < q_n)
            return shuf_at(p);
        q_shuffle_new(i);                       /* all played: another order, after i */
        return shuf_at(1);
    }
    if (i + 1 < q_n)
        return i + 1;
    return play_mode == MODE_ORDER ? -1 : 0;
}

int q_prev_index(int i)                  /* the track before i (BACK), or -1 */
{
    if (play_mode == MODE_SHUFFLE && q_n > 1) {
        int p = shuf_pos(i);
        return p > 0 ? shuf_at(p - 1) : -1;
    }
    return i - 1;
}

/* ---- building the queue ---- */
static struct fat_dir q_top;                    /* the folder held (q_tree) */
static int q_tree;

static void q_done(const char *name, uint32_t t0)
{
    q_n = 0;
    for (int f = 0; f < qd_n; f++)
        q_n += qd[f].count;
    slots_clear();
    q_shuffle_new(-1);
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
static void q_from_dir_do(int start)
{
    uint32_t t0 = ticks();
    q_clear();
    q_tree = 0;
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
                break;
            }
        break;
    }
    if (play_mode == MODE_SHUFFLE)
        q_shuffle_new(q_cur);                   /* the one chosen first, then the others */
}

void q_from_dir(int start)
{
    const char *pp = perf_begin("queue");
    q_from_dir_do(start);
    perf_end(pp);
}

/* The library list written into NEWFWQ.DAT by the storage task, while it plays */
static void write_job(void *arg)
{
    uint32_t token = (uint32_t)(uintptr_t)arg;
    int n = q_n;
    if (qf_begin()) {
        qw.state = QW_FAILED;
        return;
    }
    for (int i = 0; i < n; i++) {
        struct entry e;
        char name[NAME_KEEP + 1];
        if (q_token != token || libv_track_id(ids[i], &e, name, sizeof name) || qf_put(&e, name)) {
            qf_end(0);
            qw.state = QW_FAILED;
            return;
        }
        if (i % 8 == 7)
            storage_yield();
    }
    qw.state = q_token == token && !qf_end(1) ? QW_DONE : QW_FAILED;
    log_printf("queue: %d tracks written into NEWFWQ.DAT (%s)", n,
               qw.state == QW_DONE ? "in use" : "dropped");
}

/* A library list (libbrowse.c collects its tracks): from its track 'start', or -1 (PLAY held:
 * the first, or anywhere in shuffle) */
void q_from_lib(int kind, uint32_t param, int start, const char *name)
{
    uint32_t t0 = ticks();
    q_clear();                                  /* (a list being written: its token is old, */
    storage_drain();                            /* it stops at its next track) */
    q_tree = 0;
    int on_sd = 0;
    int n = (int)libv_collect(kind, param, ids, IDS_MAX, &on_sd);
    if (n <= 0)
        return;
    q_mode = QM_IDS;
    q_sd = on_sd;
    q_n = n;
    slots_clear();
    q_shuffle_new(-1);
    q_cur = start >= 0 && start < q_n ? start : play_mode == MODE_SHUFFLE && q_n > 1 ? shuf_at(0) : 0;
    if (play_mode == MODE_SHUFFLE && start >= 0)
        q_shuffle_new(q_cur);
    copy_name(q_folder, name, sizeof q_folder);
    log_printf("ui: %d tracks of the library's list %d.%u, %u ms", q_n, kind, param,
               (ticks() - t0) / 1000);
    qw.state = QW_RUNNING;
    qw.token = q_token;
    if (storage_submit(write_job, (void *)(uintptr_t)q_token))
        qw.state = QW_FAILED;
}

/* ---- tracks added (MENU) or put next (PLAY) in the queue mode (ETUDE §128) ---- */
static const struct q_src *add_src;
static struct entry add_e __attribute__((section(".screenram")));     /* (not the BSS) */
static char add_name[NAME_KEEP + 1] __attribute__((section(".screenram")));

/* item k of the source: an entry and its name */
static int src_item(uint32_t k, struct entry *e, char *name)
{
    const struct q_src *s = add_src;
    if (s->type == QS_ONE) {
        *e = s->e;
        copy_name(name, s->name, NAME_KEEP + 1);
        return 0;
    }
    if (s->type == QS_FOLDER) {
        const struct entry *f = &ql.e[ql.order[ql_files + (int)k]];
        *e = *f;
        copy_name(name, ql.pool + f->name, NAME_KEEP + 1);
        return 0;
    }
    return libv_track_id(ids[k], e, name, NAME_KEEP + 1);
}

static int src_put(uint32_t k, void *ctx)
{
    (void)ctx;
    return src_item(k, &add_e, add_name) ? -1 : qf_put(&add_e, add_name);
}

/* The queue made the file's (the same tracks, the same order): 0, or -1 */
static int to_file(void)
{
    if (q_is_file() || !q_n)
        return 0;
    if (q_mode == QM_IDS && qw.state == QW_RUNNING) {
        storage_drain();                        /* being written: to its end */
        if (q_is_file())
            return 0;
    }
    if (qf_begin())
        return -1;
    for (int i = 0; i < q_n; i++) {             /* (folders: each read once, in order) */
        const struct entry *e = q_ent(i);
        if (!e || qf_put(e, q_name(i))) {
            qf_end(0);
            return -1;
        }
        if (i % 32 == 31)
            ui_background();
    }
    if (qf_end(1))
        return -1;
    q_mode = QM_FILE;
    q_tree = 0;
    log_printf("queue: %d tracks of the queue written into NEWFWQ.DAT", q_n);
    return 0;
}

int q_add(const struct q_src *s, int next)
{
    storage_drain();
    if (to_file())
        return Q_ENOFILE;
    uint32_t n = 1;                             /* the source's tracks */
    int on_sd = 0;
    if (s->type == QS_FOLDER) {
        struct fat_dir d = s->d;
        ql_dir = -1;
        if (list_load(&ql, &d) < 0)
            return Q_ENOFILE;
        ql_files = 0;
        while (ql_files < ql.n && (ql.e[ql.order[ql_files]].flags & F_DIR))
            ql_files++;
        n = (uint32_t)(ql.n - ql_files);
        on_sd = s->d.vol == FAT_VOL_SD;
    } else if (s->type == QS_LIB)
        n = libv_collect(s->kind, s->param, ids, IDS_MAX, &on_sd);
    else
        on_sd = (s->e.flags & F_SD) != 0;
    if (!n)
        return 0;
    if ((uint32_t)q_n + n > QF_MAX)
        return Q_EFULL;
    add_src = s;
    int empty = !q_n, r;
    uint32_t at = next && !empty ? (uint32_t)(q_dec > q_cur ? q_dec : q_cur) + 1 : (uint32_t)q_n;
    if (at > (uint32_t)q_n)
        at = (uint32_t)q_n;
    if (empty) {
        r = qf_begin();
        for (uint32_t k = 0; !r && k < n; k++)
            r = src_put(k, 0);
        r = r ? (qf_end(0), -1) : qf_end(1);
    } else if (at == (uint32_t)q_n) {
        r = qf_append();
        for (uint32_t k = 0; !r && k < n; k++)
            r = src_put(k, 0);
        r = r ? (qf_end(0), -1) : qf_end(1);
    } else
        r = qf_insert(at, n, src_put, 0);
    if (r)
        return Q_ENOFILE;
    slots_clear();
    q_mode = QM_FILE;
    q_tree = 0;
    q_sd |= on_sd;
    q_n += (int)n;
    if (empty) {
        q_cur = q_dec = 0;
        copy_name(q_folder, s->type == QS_ONE ? s->name : T("Queue"), sizeof q_folder);
    } else if (q_dec >= (int)at)
        q_dec += (int)n;                        /* (the next one already open: after it) */
    if (play_mode == MODE_SHUFFLE && q_n > 1) { /* the order made again from the track heard */
        q_shuffle_new(q_cur);
        if (next) {
            un_anchor = q_cur;
            un_at = (int)at;
            un_n = (int)n;
        }
    }
    log_printf("queue: %u tracks %s at %u (%d now)", n, next ? "put next" : "added", at, q_n);
    return (int)n;
}

/* The queue emptied, the track playing kept */
void q_clear_keep(void)
{
    storage_drain();
    const struct entry *e = (q_playing || parked) && q_cur < q_n ? q_ent(q_cur) : 0;
    if (!e) {
        q_clear();
        if (!qf_begin())
            qf_end(1);                          /* (an empty queue in the file) */
        return;
    }
    struct entry keep = *e;
    char name[NAME_KEEP + 1];
    copy_name(name, q_name(q_cur), sizeof name);
    if (qf_begin() || qf_put(&keep, name) || qf_end(1))
        return;
    q_mode = QM_FILE;
    q_tree = 0;
    q_sd = (keep.flags & F_SD) != 0;
    q_n = 1;
    q_cur = q_dec = 0;
    un_n = 0;
    slots_clear();
    q_shuffle_new(0);
    log_printf("queue: cleared, the track playing kept");
}

/* ---- the resume of a file queue (resume.c) ---- */
void q_file_state(struct q_fstate *s)
{
    s->gen = qf_gen();
    s->cur = q_cur;
    s->n = q_n;
    s->seed = q_seed;
    s->base = (uint32_t)q_base;
    s->un_anchor = un_anchor;
    s->un_at = un_at;
    s->un_n = un_n;
}

int q_file_resume(const struct q_fstate *s, const char *name)
{
    q_clear();
    if (qf_open() || qf_gen() != s->gen || (int)qf_count() != s->n || s->n <= 0 ||
        s->cur < 0 || s->cur >= s->n)
        return -1;
    q_mode = QM_FILE;
    q_tree = 0;
    q_n = s->n;
    q_cur = q_dec = s->cur;
    q_seed = s->seed;
    q_base = (int)s->base;
    un_anchor = s->un_anchor;
    un_at = s->un_at;
    un_n = s->un_n;
    q_sd = 1;                                   /* (not known: the card's way, at worst) */
    slots_clear();
    copy_name(q_folder, name, sizeof q_folder);
    return 0;
}

/* PLAY held on a folder: everything under it, the files of each folder in order, then its
 * subfolders in order, depth first (Q_LEVELS levels, Q_DIRS folders with tracks). Each folder
 * listed is replaced in the table by its subfolders (inserted right after it, which gives the
 * depth-first order) and kept only when it has tracks. */
static void q_from_tree_do(const struct fat_dir *top, const char *name, int keep)
{
    uint32_t t0 = ticks();
    int lost = 0;
    if (keep) {                                 /* the track playing stays the queue (q_single) */
        qd_n = 0;                               /* until q_done(): slot 0 keeps it, the table */
        ql_dir = -1;                            /* below is not read for it */
    } else
        q_clear();
    q_tree = 1;
    q_top = *top;
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
    if (!keep)
        q_cur = play_mode == MODE_SHUFFLE && q_n > 1 ? shuf_at(0) : 0;     /* shuffle: anywhere */
}

void q_from_tree(const struct fat_dir *top, const char *name)
{
    const char *pp = perf_begin("tree walk");
    q_from_tree_do(top, name, 0);
    perf_end(pp);
}

/* ---- the resume of a tree queue while its track plays (0.99r23, ETUDE §104): the track first,
 * as a queue of its own, then the tree walked in the storage task with that track kept as the
 * queue (q_n 1, its slot), then the place of that track in the tree found again ---- */
void q_single(const struct entry *e, const char *name, const char *folder)
{
    q_clear();
    q_tree = 1;
    q_n = 1;
    q_cur = q_dec = 0;
    slot[0].i = 0;
    slot[0].used = ++slot_clock;
    slot[0].e = *e;
    copy_name(slot[0].name, name, sizeof slot[0].name);
    copy_name(q_folder, folder, sizeof q_folder);
}

void q_from_tree_keep(const struct fat_dir *top, const char *name)
{
    const char *pp = perf_begin("tree walk");
    q_from_tree_do(top, name, 1);
    perf_end(pp);
}

/* After q_from_tree_keep(): the saved order (the same tree), the track playing found at its
 * place, else by its cluster; q_cur and q_dec on it. Its index, or -1 (then 0). */
int q_resume_fix(const struct q_state *s, uint32_t track)
{
    int i = -1;
    if (q_n == s->n) {
        q_seed = s->seed;
        q_base = (int)s->base;
        const struct entry *e = s->cur >= 0 && s->cur < q_n ? q_ent(s->cur) : 0;
        if (e && e->cluster == track)
            i = s->cur;
    }
    for (int k = 0; i < 0 && k < q_n; k++) {    /* tracks added or removed: looked for */
        const struct entry *e = q_ent(k);
        if (e && e->cluster == track)
            i = k;
    }
    q_cur = q_dec = i < 0 ? 0 : i;
    if (q_n != s->n && play_mode == MODE_SHUFFLE)
        q_shuffle_new(q_cur);
    return i;
}

/* ---- the resume state of a tree queue (resume.c) ---- */
int q_state_get(struct q_state *s)
{
    s->cluster = q_top.cluster;
    s->size = q_top.size;
    s->vol = (uint8_t)q_top.vol;
    s->contig = (uint8_t)q_top.contig;
    s->tree = (uint8_t)q_tree;
    s->pad = 0;
    s->seed = q_seed;
    s->base = (uint32_t)q_base;
    s->cur = q_cur;
    s->n = q_n;
    return q_tree && q_n;
}

/* The same tree walked again: its order is kept when it holds as many tracks (the shuffle needs
 * the same q_n); the caller checks the track at s->cur */
int q_state_set(const struct q_state *s, const char *name)
{
    struct fat_dir d;
    d.cluster = s->cluster;
    d.size = s->size;
    d.vol = s->vol;
    d.contig = s->contig;
    q_from_tree(&d, name);
    if (!q_n)
        return -1;
    if (q_n == s->n) {
        q_seed = s->seed;
        q_base = (int)s->base;
        q_cur = s->cur >= 0 && s->cur < q_n ? s->cur : 0;
    }
    return 0;
}
