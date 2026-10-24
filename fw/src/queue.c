/* Play queue (ETUDE §25): the files of a folder, or all those under one (PLAY held), and the
 * play modes */
#include "ui_priv.h"

/* ---- play queue (in .queuebuf) ---- */
struct queue q __attribute__((section(".queuebuf")));
int q_n, q_cur = -1, q_playing, q_errors;
int q_dec;                                      /* the track core B decodes; q_cur: the one heard */

/* Play modes (BACK held on the play screen): in order, repeat all, repeat one, shuffle */
int play_mode;
uint32_t rnd = 0x2545f491;

int q_next_index(int i, int user)        /* the track after i, or -1: end of the queue */
{
    if (!q_n)
        return -1;
    if (play_mode == MODE_REPEAT_ONE && !user)
        return i;
    if (play_mode == MODE_SHUFFLE && q_n > 1) {
        rnd ^= rnd << 13;                       /* xorshift, seeded by the clock below */
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        int j = (int)(rnd % (uint32_t)(q_n - 1));
        return j >= i ? j + 1 : j;              /* any other track */
    }
    if (i + 1 < q_n)
        return i + 1;
    return play_mode == MODE_ORDER ? -1 : 0;
}

/* A track at the end of the queue, its name into the pool (*used bytes so far); 0 when full */
static int q_add(const struct entry *e, const char *name, uint32_t *used)
{
    uint32_t len = 0;
    while (name[len])
        len++;
    if (*used + len + 1 > sizeof q.pool)
        return 0;
    q.e[q_n] = *e;
    q.e[q_n].name = (uint16_t)*used;
    for (uint32_t k = 0; k <= len; k++)
        q.pool[*used + k] = name[k];
    *used += len + 1;
    q_n++;
    return 1;
}

void q_from_dir(int start)               /* the playable files of the folder shown */
{
    uint32_t used = 0;
    q_n = 0;
    q_cur = -1;
    for (int i = 0; i < dir_n && q_n < Q_MAX; i++) {
        const struct entry *e = ent(i);
        if ((e->flags & F_DIR) || !e->size)     /* empty files (grey) left out */
            continue;
        if (!q_add(e, ent_name(i), &used))
            break;
        if (i >= start && q_cur < 0)            /* the one chosen, or the next if it is empty */
            q_cur = q_n - 1;
    }
    copy_name(q.folder, path[depth].name, sizeof q.folder);
    if (q_cur < 0)
        q_cur = 0;
}

const char *q_name(int i)
{
    return q.pool + q.e[i].name;
}

/* PLAY held on a folder: everything under it, the files of each folder in order, then its
 * subfolders in order, depth first (up to 8 levels, 64 subfolders a level, Q_MAX tracks). The
 * folders are read with the listing buffer: the folder shown is read again afterwards. */
void q_from_tree(const struct fat_dir *top, const char *name)
{
    static struct fat_dir sub[8][64] __attribute__((section(".queuebuf2")));
    static int nsub[8], at[8];
    uint32_t used = 0;
    int lvl = 0;
    q_n = 0;
    q_cur = 0;
    nsub[0] = 1;
    at[0] = 0;
    sub[0][0] = *top;
    while (lvl >= 0 && q_n < Q_MAX) {
        if (at[lvl] >= nsub[lvl]) {
            lvl--;
            continue;
        }
        dir_load(&sub[lvl][at[lvl]++]);
        for (int i = 0; i < dir_n && q_n < Q_MAX; i++) {
            const struct entry *e = ent(i);
            if ((e->flags & F_DIR) || !e->size)
                continue;
            if (!q_add(e, ent_name(i), &used))
                break;
        }
        if (lvl + 1 < 8) {                      /* its subfolders, next level */
            int n = 0;
            for (int i = 0; i < dir_n && n < 64; i++)
                if (ent(i)->flags & F_DIR)
                    dir_of_entry(ent(i), &sub[lvl + 1][n++]);
            if (n) {
                lvl++;
                nsub[lvl] = n;
                at[lvl] = 0;
            }
        }
    }
    copy_name(q.folder, name, sizeof q.folder);
    log_printf("ui: %d tracks under %s", q_n, name);
    dir_load(&path[depth].dir);
}
