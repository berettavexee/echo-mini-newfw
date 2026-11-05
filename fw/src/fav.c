/* Favourites (the red heart): tracks of the internal volume or of a card, kept in the sectors
 * of NEWFWLOG.TXT before its state sector (log_aux_*; nothing else is written, see
 * docs/PERSISTENCE.md). A track is known by where it is on its volume (first cluster and size),
 * and a card's tracks by the card's serial number too: with another card in, or none, only the
 * tracks of the internal volume are shown and played. Moving or replacing a file on the
 * computer makes its heart stale (it can no longer be played): heart the file again. */
#include "ui_priv.h"

struct fav {
    uint32_t cluster, size, serial;             /* cluster 0: a free slot; serial: cards only */
    uint16_t flags;                             /* as in struct entry (codec, F_SD, F_CONTIG) */
    uint8_t sd, rsv;
    char name[40];
};
#define FAV_PER 8
#define FAV_SECTORS 8                           /* 64 favourites in 8 sectors */
#define FAV_MAX (FAV_SECTORS * FAV_PER)
struct fav_sector {
    char magic[8];                              /* "NEWFWFAV" */
    struct fav e[FAV_PER];
    uint32_t sum;
    uint32_t pad[13];
};
_Static_assert(sizeof(struct fav_sector) == 512, "favourite record must be one sector");

static struct fav fav[FAV_MAX];
static int fav_ready;

static uint32_t sector_sum(const struct fav_sector *s)
{
    const uint32_t *w = (const uint32_t *)s;
    uint32_t sum = 0x46415653;
    for (unsigned i = 0; i < __builtin_offsetof(struct fav_sector, sum) / 4; i++)
        sum = (sum << 5 | sum >> 27) ^ w[i];
    return sum;
}

void fav_load(void)
{
    static struct fav_sector s;
    int n = log_aux_count();
    for (int i = 0; i < FAV_MAX; i++)
        fav[i].cluster = 0;
    for (int k = 0; k < n && k < FAV_SECTORS; k++) {
        if (log_aux_read(k, &s) || __builtin_memcmp(s.magic, "NEWFWFAV", 8) || s.sum != sector_sum(&s))
            continue;
        for (int j = 0; j < FAV_PER; j++)
            fav[k * FAV_PER + j] = s.e[j];
    }
    fav_ready = 1;
}

static int fav_save(int idx)                    /* the sector of slot idx */
{
    static struct fav_sector s;
    int k = idx / FAV_PER;
    for (unsigned i = 0; i < sizeof s / 4; i++)
        ((uint32_t *)&s)[i] = 0;
    __builtin_memcpy(s.magic, "NEWFWFAV", 8);
    for (int j = 0; j < FAV_PER; j++)
        s.e[j] = fav[k * FAV_PER + j];
    s.sum = sector_sum(&s);
    return log_aux_write(k, &s);
}

static int matches(const struct fav *f, const struct entry *e)
{
    int on_sd = (e->flags & F_SD) != 0;
    return f->cluster && f->cluster == e->cluster && f->size == e->size && f->sd == on_sd &&
           (!on_sd || f->serial == sdvol.serial);
}

int fav_find(const struct entry *e)
{
    if (!fav_ready || !e || (e->flags & F_DIR))
        return -1;
    for (int i = 0; i < FAV_MAX; i++)
        if (matches(&fav[i], e))
            return i;
    return -1;
}

/* Heart on (1) or off (0) for a track; < 0: cannot be kept (-1 no space or no log file) */
int fav_toggle(const struct entry *e, const char *name)
{
    int i = fav_find(e);
    if (log_aux_count() <= 0)
        return -1;
    if (i >= 0) {
        fav[i].cluster = 0;
        if (fav_save(i)) {
            fav_load();                         /* what the card says */
            return -1;
        }
        return 0;
    }
    for (i = 0; i < FAV_MAX && fav[i].cluster; i++)
        ;
    if (i >= FAV_MAX || !e->cluster)
        return -1;
    struct fav *f = &fav[i];
    f->cluster = e->cluster;
    f->size = e->size;
    f->flags = e->flags;
    f->sd = (e->flags & F_SD) != 0;
    f->serial = f->sd ? sdvol.serial : 0;
    f->rsv = 0;
    copy_name(f->name, name, sizeof f->name);
    if (fav_save(i)) {
        f->cluster = 0;
        return -1;
    }
    return 1;
}

/* Shown and played: the internal volume's, and the card's when it is the card in */
static int visible(const struct fav *f)
{
    if (!f->cluster)
        return 0;
    return !f->sd || (sdvol.mounted && sd_present() && sdvol.serial == f->serial);
}

int fav_count(void)
{
    int n = 0;
    for (int i = 0; i < FAV_MAX; i++)
        n += visible(&fav[i]);
    return n;
}

/* The queue of the favourites: 0, or the number of tracks */
int fav_play_queue(const char *title)
{
    if (!fav_count())
        return 0;                               /* do not clear the current queue for an empty list */
    q_list_begin(title);
    for (int i = 0; i < FAV_MAX; i++) {
        if (!visible(&fav[i]))
            continue;
        struct entry e = { fav[i].cluster, fav[i].size, 0, fav[i].flags };
        q_list_add(&e, fav[i].name);
    }
    return q_list_end();
}
