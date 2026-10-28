/* Resume state (ETUDE §25, §38): the folder, the selection and the track (where it was), the
 * volume and the modes, kept in the last sector of the log file (log_state_*) */
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
};
static uint32_t rs_buf[128];
static uint32_t rs_last_sum;

static uint32_t rs_sum(const struct resume *r)
{
    const uint32_t *w = (const uint32_t *)r;
    uint32_t s = 0x4e465753;
    for (unsigned i = 0; i < offsetof(struct resume, sum) / 4; i++)
        s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

void resume_save(void)
{
    struct resume *r = (struct resume *)rs_buf;
    for (int i = 0; i < 128; i++)
        rs_buf[i] = 0x20202020;                 /* spaces: the log reader shows nothing */
    __builtin_memcpy(r->magic, "NEWFWSTATE 3", 12);
    r->depth = (uint32_t)depth | (depth ? (uint32_t)path[1].dir.vol << 8 : 0);
    for (int i = 0; i < DEPTH_MAX; i++) {
        r->cluster[i] = i <= depth ? path[i].dir.cluster : 0;
        r->sel[i] = (uint16_t)(i <= depth ? path[i].sel : 0);
        r->top[i] = (uint16_t)(i <= depth ? path[i].top : 0);
    }
    r->track = (q_playing || parked) && q_cur < q_n ? q.e[q_cur].cluster : 0;
    r->playing = (uint32_t)(q_playing && !audio_paused);
    r->level = (uint32_t)play_level;
    r->mode = (uint32_t)(play_mode | rg_mode << 8);    /* rg_mode 0 (album) before 0.55 */
    r->pos_ms = parked ? parked_ms : q_playing ? player_heard.time_ms : 0;
    r->sum = rs_sum(r);
    if (r->sum == rs_last_sum)
        return;                                 /* nothing changed: no write */
    rs_last_sum = r->sum;
    log_state_write(rs_buf);
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

/* At boot: back to the last folder, its selection, the track (where it was) */
void resume_load(void)
{
    struct resume *r = (struct resume *)rs_buf;
    if (log_state_read(rs_buf) || r->sum != rs_sum(r) ||
        (__builtin_memcmp(r->magic, "NEWFWSTATE 3", 12) &&
         __builtin_memcmp(r->magic, "NEWFWSTATE 2", 12)) || (r->depth & 0xff) >= DEPTH_MAX - 1)
        return;
    /* state 2 (before 0.58): no top level, index 0 was the internal root */
    int old = r->magic[11] == '2', saved = (int)(r->depth & 0xff) + old, v = (int)(r->depth >> 8);
    rs_last_sum = r->sum;
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
        path[0].sel = old ? 0 : r->sel[0];
        dir_load(&path[0].dir);
        if (path[0].sel >= dir_n)
            path[0].sel = 0;
        log_printf("ui: resume: top level (%d saved)", saved);
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
    if (!r->track || depth != saved)
        return;
    for (int i = 0; i < dir_n; i++)
        if (!(ent(i)->flags & F_DIR) && ent(i)->cluster == r->track) {
            path[depth].sel = i;
            if (path[depth].sel < path[depth].top || path[depth].sel >= path[depth].top + ROWS)
                path[depth].top = i > ROWS / 2 ? i - ROWS / 2 : 0;
            q_from_dir(i);
            if (r->playing) {                   /* where it was, two seconds before */
                player_seek_hint(r->pos_ms > 2000 ? r->pos_ms - 2000 : 0);
                play_index(q_cur);
            } else if (r->pos_ms) {             /* paused: PLAY goes on from there */
                parked = 1;
                parked_ms = r->pos_ms;
            }
            return;
        }
}
