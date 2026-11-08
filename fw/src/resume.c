/* Resume state (ETUDE §25, §38): the folder, the selection and the track (where it was), the
 * volume and the modes, kept in the last sector of the log file (log_state_*), the settings
 * (settings.c) at word 64 of it */
#include "ui_priv.h"

#define offsetof(t, m) __builtin_offsetof(t, m)

/* ---- resume (the last sector of the log file, log_state_*) ---- */
struct resume {
    char magic[12];                             /* "NEWFWSTATE 2" */
    uint32_t depth;
    uint32_t cluster[DEPTH_MAX];
    uint16_t sel[DEPTH_MAX], top[DEPTH_MAX];
    uint32_t track;                             /* cluster of the track, 0: none */
    uint32_t playing, level, mode, pos_ms, sum;
    uint32_t len_ms, len_sum;                   /* the track's length (0.99r16), for the bar of
                                                 * a parked track; its own sum: older states */
};
static uint32_t rs_buf[128];
static uint32_t rs_last_sum;

/* A queue made by PLAY held (a tree): after the resume state, with its own magic and sum, so
 * that a state written before it (0.99) still reads; the settings start at word 64 */
#define RQ_WORD 44
struct resume_q {
    char magic[4];                              /* "QTR1" */
    struct q_state q;
    char name[32];                              /* the folder held, shown on the play screen */
    uint32_t sum;
};
_Static_assert(RQ_WORD * 4 + sizeof(struct resume_q) <= 64 * 4, "resume_q overlaps the settings");
_Static_assert(sizeof(struct resume) <= RQ_WORD * 4, "resume overlaps resume_q");

/* The track of a tree queue, to play it at power-on before its tree is walked again (0.99r23,
 * ETUDE §104): words 104 to 127, after the settings (settings.c: 40 words at most) */
#define RT_WORD 104
struct resume_t {
    char magic[4];                              /* "TRK1" */
    uint32_t cluster, size;
    uint16_t flags, pad;
    char name[NAME_KEEP + 4];
    uint32_t sum;
};
_Static_assert(RT_WORD * 4 + sizeof(struct resume_t) <= 512, "resume_t past the sector");

static uint32_t rt_sum(const struct resume_t *t)
{
    const uint32_t *w = (const uint32_t *)t;
    uint32_t h = 0x4e465754;
    for (unsigned i = 0; i < offsetof(struct resume_t, sum) / 4; i++)
        h = (h << 5 | h >> 27) ^ w[i];
    return h;
}

static uint32_t rq_sum(const struct resume_q *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t s = 0x51545231;
    for (unsigned i = 0; i < offsetof(struct resume_q, sum) / 4; i++)
        s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

static uint32_t rs_sum(const struct resume *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t s = 0x4e465753;
    for (unsigned i = 0; i < offsetof(struct resume, sum) / 4; i++)
        s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

static void resume_save_do(void)
{
    if (browser_walking())
        return;                                 /* a queue being walked: the state on disk kept */
    struct resume *r = (struct resume *)rs_buf;
    for (int i = 0; i < 128; i++)
        rs_buf[i] = 0x20202020;                 /* spaces: the log reader shows nothing */
    __builtin_memcpy(r->magic, "NEWFWSTATE 4", 12);     /* 4: the library first (§127) */
    r->depth = (uint32_t)depth | (depth ? (uint32_t)path[1].dir.vol << 8 : 0);
    for (int i = 0; i < DEPTH_MAX; i++) {
        r->cluster[i] = i <= depth ? path[i].dir.cluster : 0;
        r->sel[i] = (uint16_t)(i <= depth ? path[i].sel : 0);
        r->top[i] = (uint16_t)(i <= depth ? path[i].top : 0);
    }
    const struct entry *qe = (q_playing || parked) && q_cur < q_n && !q_is_lib() ?
                             q_ent(q_cur) : 0;  /* (a library list: not resumed yet, step 5) */
    r->track = qe ? qe->cluster : 0;
    r->playing = (uint32_t)(q_playing && !audio_paused);
    r->level = (uint32_t)play_level;
    r->mode = (uint32_t)(play_mode | rg_mode << 8);    /* rg_mode 0 (album) before 0.55 */
    r->pos_ms = parked ? parked_ms : q_playing ? player_heard.time_ms : 0;
    r->sum = rs_sum(r);
    r->len_ms = parked ? parked_len : q_playing ? player_heard.length_ms : 0;
    r->len_sum = ~r->len_ms ^ r->sum;
    struct resume_q *rq = (struct resume_q *)(rs_buf + RQ_WORD);
    uint32_t tsum = 0;
    if (qe && q_state_get(&rq->q)) {
        __builtin_memcpy(rq->magic, "QTR1", 4);
        copy_name(rq->name, q_folder, sizeof rq->name);
        rq->sum = rq_sum(rq);
        struct resume_t *rt = (struct resume_t *)(rs_buf + RT_WORD);
        __builtin_memcpy(rt->magic, "TRK1", 4);
        rt->cluster = qe->cluster;
        rt->size = qe->size;
        rt->flags = qe->flags;
        rt->pad = 0;
        __builtin_memset(rt->name, 0, sizeof rt->name);
        copy_name(rt->name, q_name(q_cur), sizeof rt->name);
        rt->sum = rt_sum(rt);
        tsum = rt->sum;
    }
    uint32_t sum = r->sum ^ rq->sum ^ tsum ^ settings_put(rs_buf + 64);
    if (sum == rs_last_sum)
        return;                                 /* nothing changed: no write */
    rs_last_sum = sum;
    log_state_write(rs_buf);
}

static int rs_later;                            /* a save asked for the loop (below) */

void resume_save(void)
{
    rs_later = 0;
    const char *pp = perf_begin("resume save");
    resume_save_do();
    perf_end(pp);
}

/* A save asked by a key or a track change is written by the main loop once the frame is drawn
 * (rework step 3, ETUDE §100): the key shows at once; a power off writes it at once (rs_later
 * above) */

void resume_save_later(void)
{
    rs_later = 1;
}

void resume_flush(void)
{
    if (rs_later)
        resume_save();
}

void ui_power_off(void)
{
    resume_save();
}

static char *find_name;
static uint32_t find_cluster;
static struct fat_dir *find_dir;
static int find_entry(const struct fat_dirent *d, void *ctx)
{
    (void)ctx;
    if ((d->attr & FAT_ATTR_DIR) && d->cluster == find_cluster) {
        copy_name(find_name, d->name, 64);
        fat_dir_of(d, find_dir);
        return 1;
    }
    return 0;
}

/* The queue's track q_cur again, where it was: playing (the setting) or parked */
static void resume_track(const struct resume *r)
{
    uint32_t pos = r->playing && r->pos_ms > 2000 ? r->pos_ms - 2000 : r->playing ? 0 :
                   r->pos_ms;                   /* playing: two seconds before */
    if (r->playing && cfg.start == START_PLAY) {
        player_seek_hint(pos);
        play_index(q_cur);
    } else {                                    /* paused: PLAY goes on from there (even at 0) */
        parked = 1;
        parked_ms = pos;
        parked_len = r->len_sum == (~r->len_ms ^ r->sum) ? r->len_ms : 0;   /* 0: no bar */
        log_printf("ui: resume: paused at %u ms of %u", pos, parked_len);
    }
}

/* A tree queue (PLAY held) walked again, its track found (by its place, else by its cluster) */
static void resume_tree(const struct resume *r, struct resume_q *rq)
{
    rq->name[sizeof rq->name - 1] = 0;
    struct resume_t *rt = (struct resume_t *)(rs_buf + RT_WORD);
    if (!__builtin_memcmp(rt->magic, "TRK1", 4) && rt->sum == rt_sum(rt) &&
        rt->cluster == r->track && rt->size && !(rt->flags & F_DIR) &&
        (!(rt->flags & F_SD) || !sd_open())) {  /* its track first, the tree walked meanwhile */
        rt->name[sizeof rt->name - 1] = 0;
        struct entry e = { rt->cluster, rt->size, 0, rt->flags };
        struct fat_dir d = { .cluster = rq->q.cluster, .size = rq->q.size, .vol = rq->q.vol,
                             .contig = rq->q.contig };
        q_single(&e, rt->name, rq->name);
        if (!browser_walk_resume(&d, rq->name, &rq->q, r->track)) {
            log_printf("ui: resume: %s, its tree under %s walked meanwhile", rt->name, rq->name);
            resume_track(r);
            return;
        }
        q_clear();
    }
    if ((rq->q.vol == FAT_VOL_SD && sd_open()) || q_state_set(&rq->q, rq->name)) {
        log_printf("ui: resume: the tree queue is gone");
        return;
    }
    int i = q_cur;
    const struct entry *e = q_ent(i);
    if (!e || e->cluster != r->track) {         /* tracks added or removed: look for it */
        for (i = 0; i < q_n; i++)
            if ((e = q_ent(i)) && e->cluster == r->track)
                break;
        if (i == q_n) {
            log_printf("ui: resume: track %x not in the tree queue (%d tracks)", r->track, q_n);
            q_clear();
            return;
        }
        q_cur = i;
    }
    log_printf("ui: resume: tree queue under %s, track %d/%d%s", rq->name, q_cur + 1, q_n,
               q_n == rq->q.n ? "" : " (the tree changed: order from here)");
    if (q_n != rq->q.n && play_mode == MODE_SHUFFLE)
        q_shuffle_new(q_cur);
    resume_track(r);
}

/* At boot: back to the last folder, its selection, the track (where it was); a tree queue
 * (PLAY held) walked again */
void resume_load(void)
{
    struct resume *r = (struct resume *)rs_buf;
    int bad = log_state_read(rs_buf);
    settings_get(rs_buf + 64);                  /* the defaults when there are none */
    if (bad || r->sum != rs_sum(r) ||
        (__builtin_memcmp(r->magic, "NEWFWSTATE 4", 12) &&
         __builtin_memcmp(r->magic, "NEWFWSTATE 3", 12) &&
         __builtin_memcmp(r->magic, "NEWFWSTATE 2", 12)) || (r->depth & 0xff) >= DEPTH_MAX - 1)
        return;
    /* state 2 (before 0.58): no top level, index 0 was the internal root */
    int old = r->magic[11] == '2', saved = (int)(r->depth & 0xff) + old, v = (int)(r->depth >> 8);
    struct resume_q *rq = (struct resume_q *)(rs_buf + RQ_WORD);
    struct resume_t *rt = (struct resume_t *)(rs_buf + RT_WORD);
    rs_last_sum = r->sum ^ rq->sum ^ (rt->sum == rt_sum(rt) ? rt->sum : 0) ^
                  settings_put(rs_buf + 64);
    int tree = !__builtin_memcmp(rq->magic, "QTR1", 4) && rq->sum == rq_sum(rq) && rq->q.tree;
    if (r->level <= 0xfe)
        play_level = (int)r->level;
    if ((r->mode & 0xff) < MODE_COUNT)
        play_mode = (int)(r->mode & 0xff);
    if ((r->mode >> 8) < RG_MODES)
        rg_mode = (int)(r->mode >> 8);
    if (old)
        v = FAT_VOL_INT;
    if (!saved || v > FAT_VOL_SD || (v == FAT_VOL_SD && sd_open())) {
        depth = 0;                              /* the top level (or the card is gone) */
        path[0].sel = old ? 1 : r->sel[0] + (r->magic[11] == '3');   /* (before 4: no
                                                                         * library line) */
        dir_load(&path[0].dir);
        if (path[0].sel >= dir_n)
            path[0].sel = 0;
        log_printf("ui: resume: top level (%d saved)", saved);
        if (tree && r->track && cfg.start != START_BROWSER)
            resume_tree(r, rq);
        return;
    }
    path_root(v);
    path[1].sel = r->sel[1 - old];
    path[1].top = r->top[1 - old];
    for (int i = 2; i <= saved; i++) {          /* the folder names, from their parents */
        path[i].name[0] = 0;
        find_name = path[i].name;
        find_dir = &path[i].dir;
        find_cluster = r->cluster[i - old];
        fat_list(&path[i - 1].dir, find_entry, 0);
        if (!path[i].name[0])
            break;                              /* gone: stop at the parent */
        path[i].sel = r->sel[i - old];
        path[i].top = r->top[i - old];
        depth = i;
    }
    dir_load(&path[depth].dir);
    if (path[depth].sel >= dir_n)
        path[depth].sel = path[depth].top = 0;
    log_printf("ui: resume: %s, depth %d (%d saved), track %x at %u ms, playing %u, -%u dB",
               v == FAT_VOL_SD ? "card" : "internal", depth, saved, r->track, r->pos_ms,
               r->playing, (uint32_t)play_level / 2);
    if (!r->track || cfg.start == START_BROWSER)
        return;                                 /* (the setting: the folder only) */
    if (tree) {
        resume_tree(r, rq);
        return;
    }
    if (depth != saved)
        return;
    for (int i = 0; i < dir_n; i++)
        if (!(ent(i)->flags & F_DIR) && ent(i)->cluster == r->track) {
            path[depth].sel = i;
            if (path[depth].sel < path[depth].top || path[depth].sel >= path[depth].top + ROWS)
                path[depth].top = i > ROWS / 2 ? i - ROWS / 2 : 0;
            q_from_dir(i);
            resume_track(r);
            return;
        }
}
