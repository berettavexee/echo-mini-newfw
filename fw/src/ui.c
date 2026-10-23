/* User interface (step 6, ETUDE §25): folder browser, now playing, main menu, play queue.
 * Modelled on the behaviour of RECHO's interface (no code of it): folders first, then files,
 * natural order ignoring case; VOL+/VOL- move (held: repeat), PLAY opens or plays, BACK goes up,
 * MENU switches between the browser and now playing, MENU held opens the main menu. Playback
 * runs in the background: the main loop services the player between two key polls. */
#include <stdint.h>
#include "audio.h"
#include "board.h"
#include "fat.h"
#include "hifi.h"
#include "hifi_proto.h"
#include "lcd.h"
#include "log.h"
#include "player.h"
#include "rtc.h"
#include "sd.h"
#include "sys.h"
#include "tags.h"
#include "ui.h"

#define offsetof(t, m) __builtin_offsetof(t, m)

#define TITLE_FG RGB(255, 210, 90)
#define DIR_FG   RGB(140, 200, 255)
#define SEL_BG   RGB(40, 70, 140)
#define BAR_FG   RGB(90, 160, 255)
#define RG_TRACK_BG RGB(240, 200, 0)            /* ReplayGain of the track (not the album) */

/* ---- keys: short press (on release), long press (held 600 ms), repeat for VOL+/VOL- ----
 * VOL+ and VOL- together (the chord): VOL- grounds the ladder, so the pair reads as VOL-. It
 * shows as a change from one volume key to the other with no release between them (held 100 ms,
 * key_scan): EV_CHORD, *key the key pressed first, then nothing until both are released. Since
 * 0.53 a short press of VOL+/VOL- is also taken on release (a chord may follow it: no volume step
 * to undo); held, it repeats from 450 ms, without a step on release. */
enum { EV_NONE, EV_PRESS, EV_LONG, EV_REPEAT, EV_CHORD };
static int kp_key, kp_long, kp_rep, kp_chord;
static uint32_t kp_t0, kp_next;

static int is_vol(int k)
{
    return k == KEY_VOLUP || k == KEY_VOLDOWN;
}

static int key_poll(int *key)
{
    int k = key_scan();
    uint32_t now = ticks();
    if (k != kp_key) {
        int old = kp_key, ev = EV_NONE;
        if (old && !kp_long && !kp_rep && !kp_chord && !(is_vol(old) && is_vol(k))) {
            *key = old;
            ev = EV_PRESS;
            log_printf("keys: %s (adc %u, %u ms)", key_name(old), key_adc, (now - kp_t0) / 1000);
        }
        kp_key = k;
        kp_t0 = now;
        kp_long = kp_rep = 0;
        if (!k)
            kp_chord = 0;
        if (kp_chord)
            return EV_NONE;
        if (is_vol(old) && is_vol(k)) {
            kp_chord = 1;
            *key = old;
            log_printf("keys: chord vol+- (%s first, adc %u)", key_name(old), key_adc);
            return EV_CHORD;
        }
        if (k && is_vol(k))
            kp_next = now + 450000;
        return ev;
    }
    if (!k || kp_chord)
        return EV_NONE;
    if (is_vol(k)) {
        if ((int32_t)(now - kp_next) >= 0) {
            if (!kp_rep)
                log_printf("keys: %s held (adc %u)", key_name(k), key_adc);
            kp_rep = 1;
            kp_next = now + 90000;
            *key = k;
            return EV_REPEAT;
        }
    } else if (!kp_long && now - kp_t0 > 600000) {
        kp_long = 1;
        *key = k;
        log_printf("keys: %s long (adc %u)", key_name(k), key_adc);
        return EV_LONG;
    }
    return EV_NONE;
}

/* ---- names ---- */
static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* Natural order, ignoring ASCII case: digit runs compare as numbers ("2" before "10") */
static int name_cmp(const char *a, const char *b)
{
    for (;;) {
        unsigned ca = (uint8_t)*a, cb = (uint8_t)*b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            while (*a == '0')
                a++;
            while (*b == '0')
                b++;
            const char *ea = a, *eb = b;
            while (*ea >= '0' && *ea <= '9')
                ea++;
            while (*eb >= '0' && *eb <= '9')
                eb++;
            if (ea - a != eb - b)
                return (int)(ea - a) - (int)(eb - b);
            for (; a < ea; a++, b++)
                if (*a != *b)
                    return *a - *b;
            continue;
        }
        if (!ca || !cb)
            return (int)ca - (int)cb;
        int la = lower((int)ca), lb = lower((int)cb);
        if (la != lb)
            return la - lb;
        a++;
        b++;
    }
}

/* Our codec for a file name (by its extension), 0 when we do not play it */
static uint32_t codec_of(const char *name)
{
    static const struct { const char *ext; uint32_t codec; } t[] = {
        { "wav", HP_CODEC_WAV }, { "flac", HP_CODEC_FLAC }, { "fla", HP_CODEC_FLAC },
        { "mp3", HP_CODEC_MP3 }, { "ogg", HP_CODEC_VORBIS }, { "oga", HP_CODEC_VORBIS },
        { "opus", HP_CODEC_OPUS }, { "opu", HP_CODEC_OPUS },
    };
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.')
            dot = p;
    if (!dot)
        return 0;
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        const char *e = t[i].ext, *p = dot + 1;
        while (*e && lower(*p) == *e) {
            e++;
            p++;
        }
        if (!*e && !*p)
            return t[i].codec;
    }
    return 0;
}

/* ---- directory listing (in .dirbuf) ---- */
#define DIR_MAX  1000
#define DIR_POOL 81920
#define F_DIR     0x8000
#define F_SD      0x4000                        /* on the card (else the internal volume) */
#define F_CONTIG  0x2000                        /* exFAT "no FAT chain" */
#define F_VOLROOT 0x1000                        /* the top level: a volume */
#define F_CODEC   0x0fff
#define NAME_KEEP 72                            /* bytes of a name kept (the screen shows ~40) */
struct entry {
    uint32_t cluster, size;
    uint16_t name;              /* offset in the pool */
    uint16_t flags;             /* F_DIR, or the codec; F_SD, F_CONTIG, F_VOLROOT */
};
static struct {
    struct entry e[DIR_MAX];
    uint16_t order[DIR_MAX];
    char pool[DIR_POOL];
} dir __attribute__((section(".dirbuf")));
static int dir_n, dir_full;
static uint32_t dir_pool_used, dir_cluster;

static const char *ent_name(int i)
{
    return dir.pool + dir.e[dir.order[i]].name;
}

static const struct entry *ent(int i)
{
    return &dir.e[dir.order[i]];
}

static int collect(const struct fat_dirent *d, void *ctx)
{
    (void)ctx;
    uint32_t codec = d->attr & FAT_ATTR_DIR ? 0 : codec_of(d->name);
    if (!(d->attr & FAT_ATTR_DIR) && !codec)
        return 0;                               /* not something we play */
    /* a big folder: files stop at DIR_MAX - 50, the last places are kept for folders, and the
     * whole directory is still read so that no folder goes missing */
    if (dir_n >= (d->attr & FAT_ATTR_DIR ? DIR_MAX : DIR_MAX - 50)) {
        dir_full = 1;
        return 0;
    }
    uint32_t len = 0;
    while (d->name[len])
        len++;
    if (len > NAME_KEEP) {                      /* more than the screen shows: cut on a */
        len = NAME_KEEP;                        /* character boundary */
        while (len && ((uint8_t)d->name[len] & 0xc0) == 0x80)
            len--;
    }
    if (dir_pool_used + len + 1 > DIR_POOL) {
        dir_full = 1;
        return 0;
    }
    struct entry *e = &dir.e[dir_n];
    e->cluster = d->cluster;
    e->size = d->size;
    e->name = (uint16_t)dir_pool_used;
    e->flags = (uint16_t)((d->attr & FAT_ATTR_DIR ? F_DIR : codec) |
                          (d->vol == FAT_VOL_SD ? F_SD : 0) | (d->contig ? F_CONTIG : 0));
    for (uint32_t i = 0; i < len; i++)
        dir.pool[dir_pool_used + i] = d->name[i];
    dir.pool[dir_pool_used + len] = 0;
    dir_pool_used += len + 1;
    dir.order[dir_n] = (uint16_t)dir_n;
    dir_n++;
    if (!(dir_n & 31))
        ui_background();                        /* keep the music going while we read */
    return 0;
}

static int before(int a, int b)                 /* entries a, b by index in dir.e */
{
    int da = dir.e[a].flags & F_DIR, db = dir.e[b].flags & F_DIR;
    if (da != db)
        return da > db;
    return name_cmp(dir.pool + dir.e[a].name, dir.pool + dir.e[b].name) < 0;
}

static void dir_sort(void)
{
    static const int gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };  /* Ciura */
    for (unsigned g = 0; g < sizeof gaps / sizeof *gaps; g++) {
        int gap = gaps[g];
        for (int i = gap; i < dir_n; i++) {
            uint16_t t = dir.order[i];
            int j = i;
            for (; j >= gap && before(t, dir.order[j - gap]); j -= gap)
                dir.order[j] = dir.order[j - gap];
            dir.order[j] = t;
        }
        ui_background();
    }
}

/* The top level (ETUDE §38): the internal volume, then the card when one is in */
#define VOL_TOP 2                               /* fat_dir.vol of the top level */
static int sd_open(void);
static int sd_bad;                              /* the card in did not mount */

static void top_entry(const char *name, uint16_t flags)
{
    struct entry *e = &dir.e[dir_n];
    uint32_t len = 0;
    while (name[len])
        len++;
    e->cluster = e->size = 0;
    e->name = (uint16_t)dir_pool_used;
    e->flags = (uint16_t)(F_DIR | F_VOLROOT | flags);
    for (uint32_t i = 0; i <= len; i++)
        dir.pool[dir_pool_used + i] = name[i];
    dir_pool_used += len + 1;
    dir.order[dir_n] = (uint16_t)dir_n;
    dir_n++;
}

static int dir_load(const struct fat_dir *d)
{
    uint32_t t0 = ticks();
    dir_n = dir_full = 0;
    dir_pool_used = 0;
    dir_cluster = d->cluster;
    if (d->vol == VOL_TOP) {
        top_entry("Internal", 0);
        if (sd_present())
            top_entry(sd_bad ? "SD card (not readable)" : "SD card", F_SD);
        return 0;
    }
    if (d->vol == FAT_VOL_SD && sd_open())
        return -1;
    int r = fat_list(d, collect, 0);
    uint32_t t1 = ticks();
    dir_sort();
    log_printf("ui: folder %s%x: %d entries%s, read %u ms, sorted %u ms",
               d->vol == FAT_VOL_SD ? "sd:" : "", d->cluster, dir_n,
               dir_full ? " (more not shown)" : "", (t1 - t0) / 1000, (ticks() - t1) / 1000);
    return r;
}

/* The directory an entry of the listing designates */
static void dir_of_entry(const struct entry *e, struct fat_dir *d)
{
    d->vol = e->flags & F_SD ? FAT_VOL_SD : FAT_VOL_INT;
    d->contig = (e->flags & F_CONTIG) != 0;
    d->cluster = e->flags & F_VOLROOT ? 0 : e->cluster;
    d->size = e->flags & F_VOLROOT ? 0 : e->size;
}

static int same_file(const struct entry *a, const struct entry *b)
{
    return a->cluster == b->cluster && (a->flags & F_SD) == (b->flags & F_SD);
}

/* ---- browser state: the path from the root ---- */
#define DEPTH_MAX 16
static struct {
    struct fat_dir dir;         /* path[0]: the top level (VOL_TOP), path[1]: a volume's root */
    int sel, top;
    char name[64];
} path[DEPTH_MAX];
static int depth;                               /* path[depth] is the folder shown */

static void copy_name(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    while (i > 0 && ((uint8_t)s[i] & 0xc0) == 0x80)
        i--;                                    /* cut before a character, not inside it */
    d[i] = 0;
}

/* ---- play queue (in .queuebuf) ---- */
#define Q_MAX  400
#define Q_POOL 20480
static struct {
    struct entry e[Q_MAX];
    char pool[Q_POOL];
    char folder[64];
} q __attribute__((section(".queuebuf")));
static int q_n, q_cur = -1, q_playing, q_errors;
static int q_dec;                               /* the track core B decodes; q_cur: the one heard */

/* Play modes (BACK held on the play screen): in order, repeat all, repeat one, shuffle */
enum { MODE_ORDER, MODE_REPEAT_ALL, MODE_REPEAT_ONE, MODE_SHUFFLE, MODE_COUNT };
static int play_mode;
static uint32_t rnd = 0x2545f491;

static int q_next_index(int i, int user)        /* the track after i, or -1: end of the queue */
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

static void q_from_dir(int start)               /* the playable files of the folder shown */
{
    uint32_t used = 0;
    q_n = 0;
    q_cur = -1;
    for (int i = 0; i < dir_n && q_n < Q_MAX; i++) {
        const struct entry *e = ent(i);
        if (e->flags & F_DIR)
            continue;
        const char *nm = ent_name(i);
        uint32_t len = 0;
        while (nm[len])
            len++;
        if (used + len + 1 > Q_POOL)
            break;
        if (i == start)
            q_cur = q_n;
        q.e[q_n] = *e;
        q.e[q_n].name = (uint16_t)used;
        for (uint32_t k = 0; k <= len; k++)
            q.pool[used + k] = nm[k];
        used += len + 1;
        q_n++;
    }
    copy_name(q.folder, path[depth].name, sizeof q.folder);
    if (q_cur < 0)
        q_cur = 0;
}

static const char *q_name(int i)
{
    return q.pool + q.e[i].name;
}

/* PLAY held on a folder: everything under it, the files of each folder in order, then its
 * subfolders in order, depth first (up to 8 levels, 64 subfolders a level, Q_MAX tracks). The
 * folders are read with the listing buffer: the folder shown is read again afterwards. */
static void q_from_tree(const struct fat_dir *top, const char *name)
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
            if (e->flags & F_DIR)
                continue;
            const char *nm = ent_name(i);
            uint32_t len = 0;
            while (nm[len])
                len++;
            if (used + len + 1 > Q_POOL)
                break;
            q.e[q_n] = *e;
            q.e[q_n].name = (uint16_t)used;
            for (uint32_t k = 0; k <= len; k++)
                q.pool[used + k] = nm[k];
            used += len + 1;
            q_n++;
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

static void resume_save(void);

/* ---- playback ---- */
int play_level = 80;                            /* PCM_VOL: attenuation / 2 dB, start at -40 dB */
static struct tags tg, tg_next;                 /* of the track heard, of the one opened last */
static int fresh;                               /* the output was (re)started: fade in once fed */
static int draining;                            /* end of the queue: the DAC ring plays out */
static uint32_t heard_gen;

/* ReplayGain (ETUDE §35): through the DAC volume (0.5 dB steps, no work on the samples), from the
 * moment the track is heard. Album mode takes the track gain when there is no album gain; no tag:
 * 0 dB; no preamp. Limited by the peak (when tagged) and by 0 dB of attenuation. */
enum { RG_MODE_ALBUM, RG_MODE_TRACK, RG_MODE_OFF, RG_MODES };
static const char *const rg_names[RG_MODES] = { "album", "track", "off" };
static int rg_mode;
static int rg_steps;                            /* applied to the track heard, 0.5 dB (+: louder) */
static int rg_tag = RG_NONE;                    /* its tag, 1/100 dB */
static int rg_src;                              /* RG_TRACK or RG_ALBUM: the gain used */

static int out_level(void)                      /* the DAC attenuation: the volume, then the gain */
{
    int v = play_level - rg_steps;
    return v < 0 ? 0 : v > 0xfe ? 0xfe : v;
}

static void rg_apply(void)
{
    int g = RG_NONE, steps = 0, a = RG_ALBUM;
    uint32_t pk = 0;
    if (rg_mode != RG_MODE_OFF) {
        a = rg_mode == RG_MODE_ALBUM && tg.rg_gain[RG_ALBUM] != RG_NONE ? RG_ALBUM : RG_TRACK;
        g = tg.rg_gain[a];
        pk = tg.rg_peak[a] ? tg.rg_peak[a] : tg.rg_peak[a ^ 1];
    }
    if (g != RG_NONE) {
        steps = (g >= 0 ? g + 25 : g - 25) / 50;
        if (pk) {                               /* headroom of the peak, 0.5 dB steps */
            uint32_t x = pk;
            int h = 0;
            if (x <= 65536)
                while (h < 120 && (uint64_t)x * 69419 >> 16 <= 65536) {    /* x 10^(0.5/20) */
                    x = (uint32_t)((uint64_t)x * 69419 >> 16);
                    h++;
                }
            else
                while (x > 65536 && h > -120) {
                    x = (uint32_t)(((uint64_t)x << 16) / 69419);
                    h--;
                }
            if (steps > h)
                steps = h;
        }
    }
    if (steps != rg_steps || g != rg_tag) {
        unsigned m = (unsigned)(steps < 0 ? -steps : steps);
        log_printf("ui: replaygain %s: %s tag %d/100 dB, peak %u/65536: %s%u.%u dB applied",
                   rg_names[rg_mode], a == RG_TRACK ? "track" : "album", g, pk,
                   steps < 0 ? "-" : "+", m / 2, m % 2 * 5);
        rg_steps = steps;
        rg_tag = g;
    }
    rg_src = g == RG_NONE ? RG_ALBUM : a;
    if (!audio.on || audio.vol == 0xff)
        return;                                 /* muted until the fade in: it takes out_level() */
    if (audio_paused)
        audio.vol = (uint8_t)out_level();       /* audio_pause(0) puts it back */
    else if (audio.vol != out_level())
        audio_volume((uint8_t)out_level(), 1);
}

/* A chained track is opened up to 1.1 s before it is heard (the DAC ring plays the end of the
 * last one meanwhile): the screen, the resume state and the keys follow the track heard */
static void heard_sync(void)
{
    if (player_heard.gen == heard_gen)
        return;
    heard_gen = player_heard.gen;
    q_cur = q_dec;
    tg = tg_next;
    rg_apply();
    resume_save();
}

static int q_open(int i, int chain)
{
    struct fat_file f;
    copy_name(f.name, q_name(i), sizeof f.name);
    f.vol = q.e[i].flags & F_SD ? FAT_VOL_SD : FAT_VOL_INT;
    f.contig = (q.e[i].flags & F_CONTIG) != 0;
    f.cluster = q.e[i].cluster;
    f.size = q.e[i].size;
    fat_open(&f);
    uint32_t t0 = ticks();
    tags_read(&f, q.e[i].flags & F_CODEC, &tg_next);
    log_printf("ui: tags in %u us: '%s' / '%s' / '%s' / %u", ticks() - t0, tg_next.title,
               tg_next.artist, tg_next.album, tg_next.track);
    int was_on = audio.on;
    (void)chain;
    player_ogg_hint(tg_next.ogg_end_page, tg_next.ogg_end_seg);  /* core B skips the comment header */
    int r = player_open(&f, q.e[i].flags & F_CODEC, PLAY_CHAIN);
    if (!r && (!was_on || audio.vol == 0xff))
        fresh = 1;
    log_printf("ui: queue %d/%d: %s (%d)", i + 1, q_n, q_name(i), r);
    if (!r) {
        q_dec = i;
        heard_sync();                           /* at once unless the last one still plays */
    }
    return r;
}

/* Parked: a long pause (PARK_MS) stops the track, core B and the DACs; PLAY starts it again
 * where it was (player_seek_hint). Also the state after a resume of a paused track. */
static int parked;
static uint32_t parked_ms;

/* Stops what plays: STOP_SKIP leaves the DACs running and drops what is queued for them (a skip),
 * STOP_DRAIN plays what is queued first (the end of the queue), STOP_NOW stops at once */
enum { STOP_NOW, STOP_SKIP, STOP_DRAIN };

static void play_stop(int how)
{
    draining = 0;
    if (how != STOP_DRAIN)
        player_drop_tail();
    if (player_is_open())
        player_close(how == STOP_SKIP ? 0 : 1);
    if (how == STOP_NOW && audio_paused) {      /* from a pause: stopped as it is, without */
        audio_stop();                           /* powering the DACs up and unmuting them */
        audio_paused = 0;
        q_playing = 0;
        return;
    }
    audio_pause(0);
    if (how == STOP_SKIP) {
        audio_flush();
        return;
    }
    if (how == STOP_DRAIN)
        player_chain_end();
    audio_stop();
    q_playing = 0;
}

static void play_index(int i)
{
    int keep = q_playing && audio.on;
    parked = 0;
    if (player_is_open() || keep)
        play_stop(keep ? STOP_SKIP : STOP_NOW);
    q_cur = q_dec = i;
    q_playing = 1;
    q_errors = 0;
    while (q_dec < q_n && q_open(q_dec, keep) < 0) {
        if (++q_errors > 8 || ++q_dec >= q_n) {
            play_stop(STOP_NOW);
            q_cur = q_dec = i;                  /* none would open: stay on the one chosen */
            return;
        }
    }
    q_cur = q_dec;                              /* chosen: shown at once, even while the DACs */
    tg = tg_next;                               /* change rate (published after it) */
    rg_apply();
}

/* The player between two key polls; called during long reads too */
void ui_background(void)
{
    if (!q_playing)
        return;
    if (fresh && audio.on && audio.vol == 0xff && audio_fill() > 16384) {
        fresh = 0;
        audio_volume((uint8_t)out_level(), 1);  /* fade in once a little audio is queued */
    }
    if (draining) {                             /* the end of the queue, heard to its last frame */
        if (!player_drain_service())
            return;
        play_stop(STOP_DRAIN);
        q_cur = q_dec = q_n ? q_n - 1 : 0;
        return;
    }
    int r = player_service();
    heard_sync();
    if (r == 0)
        return;
    if (r == 1 && audio_paused && play_mode == MODE_ORDER && q_dec + 1 >= q_n)
        return;                                 /* end of the queue in a pause: drained after it */
    player_close(0);                            /* end of the track (or an error) */
    for (int n = q_next_index(q_dec, 0); n >= 0; n = q_next_index(n, 0)) {
        q_dec = n;
        if (q_open(q_dec, 1) == 0)
            return;                             /* the next one, gapless when it can */
        if (++q_errors > 8)
            break;
    }
    draining = 1;                               /* end of the queue: followed from the loop */
}

/* ---- seek on the bar (ETUDE §31): VOL+ and VOL- together on the play screen. VOL+/VOL- move a
 * target (10 s a step; held: 30 s after 1.5 s, 1 min after 4 s, at most 4 steps a second); PLAY
 * or MENU goes there, BACK cancels, SEEK_IDLE_MS without a key goes there too. The music goes on
 * meanwhile: one reopen at the end (player_seek_hint), whatever the number of steps. From a
 * pause the track plays again there; parked, only the position kept changes. ---- */
#ifndef SEEK_IDLE_MS
#define SEEK_IDLE_MS 2000u
#endif
#define SK_FG RGB(255, 150, 60)
static int sk_on, sk_cur;
static uint32_t sk_ms, sk_from, sk_key_ms, sk_step_ms;

static uint32_t now_ms(void)                    /* the position shown */
{
    return parked ? parked_ms : player_heard.time_ms;
}

static void seek_begin(void)
{
    if (!q_n || !player_heard.length_ms || (!q_playing && !parked)) {
        log_printf("ui: no seek (nothing open, or no length)");
        return;
    }
    sk_on = 1;
    sk_cur = q_cur;
    sk_ms = sk_from = now_ms();
    sk_key_ms = millis();
    sk_step_ms = 0;
}

static void seek_move(int d, int repeat)
{
    uint32_t now = millis(), held = (ticks() - kp_t0) / 1000;
    sk_key_ms = now;
    if (repeat && now - sk_step_ms < 250)
        return;
    sk_step_ms = now;
    uint32_t step = !repeat || held < 1500 ? 10000 : held < 4000 ? 30000 : 60000;
    uint32_t max = player_heard.length_ms > 1000 ? player_heard.length_ms - 1000 : 0;
    if (d < 0)
        sk_ms = sk_ms > step ? sk_ms - step : 0;
    else
        sk_ms = sk_ms + step < max ? sk_ms + step : max;
}

static void seek_end(int go)
{
    sk_on = 0;
    uint32_t d = sk_ms > sk_from ? sk_ms - sk_from : sk_from - sk_ms;
    if (!go || d < 500 || sk_cur != q_cur || (!q_playing && !parked))
        return;
    log_printf("ui: seek %u -> %u ms of %u%s", now_ms(), sk_ms, player_heard.length_ms,
               parked ? " (parked)" : "");
    if (parked) {
        parked_ms = sk_ms;
        resume_save();
        return;
    }
    player_seek_hint(sk_ms);
    play_index(q_cur);
}

/* ---- drawing ---- */
static void put2(char **p, uint32_t v)
{
    *(*p)++ = (char)('0' + v / 10 % 10);
    *(*p)++ = (char)('0' + v % 10);
}

static char *fmt_time(char *p, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) {
        p = put_dec(p, s / 3600);
        *p++ = ':';
        put2(&p, s / 60 % 60);
    } else
        p = put_dec(p, s / 60);
    *p++ = ':';
    put2(&p, s % 60);
    *p = 0;
    return p;
}

static int bar_x(uint32_t pos, uint32_t len)
{
    return len ? (int)((uint64_t)(pos > len ? len : pos) * (LCD_W - 8) / len) : 0;
}

static void progress_bar(int y, int h, uint32_t pos, uint32_t len)
{
    int w = bar_x(pos, len);
    lcd_fill(4, y, w, h, BAR_FG);
    lcd_fill(4 + w, y, LCD_W - 8 - w, h, RGB(40, 40, 50));
}

/* The bar in the seek mode: the position dimmed, the target as a mark above and below it */
static void seek_bar(int y, int h, uint32_t pos, uint32_t target, uint32_t len)
{
    int w = bar_x(pos, len), t = 4 + bar_x(target, len);
    lcd_fill(0, y - 2, LCD_W, 2, BG);
    lcd_fill(0, y + h, LCD_W, 3, BG);
    lcd_fill(4, y, w, h, RGB(60, 90, 140));
    lcd_fill(4 + w, y, LCD_W - 8 - w, h, RGB(40, 40, 50));
    if (t > LCD_W - 6)
        t = LCD_W - 6;
    lcd_fill(t - 1, y - 2, 3, h + 5, SK_FG);
}

#define ROWS   9
#define ROW_Y0 18
static int b_dirty = 1, b_sel = -1, b_top = -1, b_mark = -1;

static int playing_index(void)                  /* the row of the track that plays, or -1 */
{
    if (!q_playing || q_cur >= q_n)
        return -1;
    for (int i = 0; i < dir_n; i++)
        if (!(ent(i)->flags & F_DIR) && same_file(ent(i), &q.e[q_cur]))
            return i;
    return -1;
}

static void draw_row(int r, int sel, int mark)
{
    int i = path[depth].top + r, y = ROW_Y0 + 16 * r;
    if (i >= dir_n) {
        lcd_fill(0, y, LCD_W, 16, BG);
        if (!dir_n && !r)
            lcd_text(8, y, "no folder or playable file here", DIM, BG);
        return;
    }
    const struct entry *e = ent(i);
    uint16_t bg = i == sel ? SEL_BG : BG;
    int dirent = e->flags & F_DIR;
    /* a fixed column for the mark: » folder, ♪ the track that plays */
    lcd_fill(0, y, 22, 16, bg);
    if (dirent || i == mark)
        lcd_glyph(dirent ? 6 : 3, y, dirent ? 0xbb : 0x266a, dirent ? DIR_FG : TITLE_FG, bg);
    lcd_text_clip(22, y, ent_name(i), LCD_W, dirent ? DIR_FG : FG, bg);
}

/* Text right-aligned at x1 in [x0, x1) of a line: the band left of it filled with bg, each pixel
 * written once (no clearing first: it would flicker, ETUDE §39) */
static void text_right(int x0, int x1, int y, const char *s, uint16_t fg, uint16_t bg)
{
    int x = x1 - lcd_text_width(s);
    if (x < x0)
        x = x0;
    if (x > x0)
        lcd_fill(x0, y, x - x0, 16, bg);
    lcd_text_clip(x, y, s, x1, fg, bg);
}

/* "HH:MM" right-aligned at x1 in [x0, x1) of the first line, once the clock has been set;
 * redrawn when the minute changes or when forced */
static int clock_min = -1;

static void draw_clock(int x0, int x1, int force)
{
    struct rtc_tm t;
    int ok = rtc_get(&t) == 1, m = ok ? t.hour * 60 + t.min : -2;
    if (!force && m == clock_min)
        return;
    clock_min = m;
    char b[8] = { (char)('0' + t.hour / 10), (char)('0' + t.hour % 10), ':',
                  (char)('0' + t.min / 10), (char)('0' + t.min % 10), 0 };
    if (!ok)
        b[0] = 0;
    text_right(x0, x1, 0, b, DIM, BG);
}

static void draw_browser(void)
{
    char b[96], *p;
    int sel = path[depth].sel, top = path[depth].top, mark = playing_index();
    if (!b_dirty && top == b_top) {
        /* the same page: only the rows whose look changed (selection, playing mark) */
        for (int r = 0; r < ROWS; r++) {
            int i = top + r;
            if (i == sel || i == b_sel || i == mark || i == b_mark)
                if ((i == sel) != (i == b_sel) || (i == mark) != (i == b_mark))
                    draw_row(r, sel, mark);
        }
        b_sel = sel;
        b_mark = mark;
        if (q_playing && player_heard.length_ms)
            progress_bar(ROW_Y0 + 16 * ROWS + 3, 4, player_heard.time_ms, player_heard.length_ms);
        draw_clock(LCD_W - 100, LCD_W - 60, 0);
        return;
    }
    if (b_dirty) {                              /* every band once, nothing cleared first */
        lcd_fill(0, 0, 4, 16, BG);
        lcd_text_clip(4, 0, depth ? path[depth].name : "Storage", LCD_W - 104, TITLE_FG, BG);
        draw_clock(LCD_W - 104, LCD_W - 60, 1);
        p = put_dec(b, (uint32_t)dir_n);
        if (dir_full)
            *p++ = '+';
        *p = 0;
        text_right(LCD_W - 60, LCD_W - 4, 0, b, DIM, BG);
        lcd_fill(LCD_W - 4, 0, 4, 16, BG);
        lcd_fill(0, 16, LCD_W, ROW_Y0 - 16, BG);
    }
    for (int r = 0; r < ROWS; r++)
        draw_row(r, sel, mark);
    b_sel = sel;
    b_top = top;
    b_mark = mark;
    /* bottom strip: the progress of what plays (around the bar: background, drawn once) */
    int yb = ROW_Y0 + 16 * ROWS;
    if (q_playing && player_heard.length_ms) {
        if (b_dirty) {
            lcd_fill(0, yb, LCD_W, 3, BG);
            lcd_fill(0, yb + 3, 4, 4, BG);
            lcd_fill(LCD_W - 4, yb + 3, 4, 4, BG);
            lcd_fill(0, yb + 7, LCD_W, LCD_H - yb - 7, BG);
        }
        progress_bar(yb + 3, 4, player_heard.time_ms, player_heard.length_ms);
    } else
        lcd_fill(0, yb, LCD_W, LCD_H - yb, BG);
    b_dirty = 0;
}

/* Battery: a Li-ion curve on the measured voltage (read every 5 s), and the USB plug: "CHG"
 * while charging, "USB" once charged */
static char *battery_text(char *p)
{
    static const uint16_t mv[] = { 3300, 3600, 3700, 3780, 3850, 3920, 4000, 4080, 4160 };
    static const uint8_t pc[] = { 0, 8, 20, 35, 50, 65, 80, 92, 100 };
    static uint32_t t_read, last_mv;
    if (!last_mv || ticks() - t_read > 5000000) {
        t_read = ticks();
        last_mv = battery_mv();
    }
    uint32_t v = 0;
    if (last_mv >= mv[8])
        v = 100;
    else
        for (int i = 1; i < 9; i++)
            if (last_mv < mv[i]) {
                v = last_mv <= mv[i - 1] ? pc[i - 1] :
                    pc[i - 1] + (last_mv - mv[i - 1]) * (pc[i] - pc[i - 1]) / (mv[i] - mv[i - 1]);
                break;
            }
    if (vbus())
        p = put_str(p, charging() ? "CHG " : "USB ");
    p = put_dec(p, v);
    return put_str(p, "%");
}

static const char *codec_name(uint32_t c)
{
    static const char *const n[] = { "", "WAV", "FLAC", "MP3", "Vorbis", "Opus" };
    return c < sizeof n / sizeof *n ? n[c] : "";
}

/* Lines of the play screen too long for it (artist, album) scroll left: 1.5 s still at the
 * start, then 1 pixel every 33 ms (30 px/s), round and round with a gap of 32 pixels */
#define MQ_X     4
#define MQ_XMAX  (LCD_W - 4)
#define MQ_GAP   32
#define MQ_PAUSE 1500000u
#define MQ_STEP  33000u
static struct marquee {
    const char *s;
    int y, w, off;
    uint16_t fg;
    uint32_t t;                                 /* start of the pause, then of the last step */
} mq[2];

static void marquee_set(struct marquee *m, int y, const char *s, uint16_t fg)
{
    m->s = s;
    m->y = y;
    m->fg = fg;
    m->off = 0;
    m->t = ticks();
    m->w = lcd_text_width(s);
    if (m->w <= MQ_XMAX - MQ_X)
        m->w = 0;                               /* it fits: drawn once, never scrolled */
    lcd_text_clip(MQ_X, y, s, MQ_XMAX, fg, BG);
}

static void marquee_tick(void)
{
    uint32_t now = ticks();
    for (int i = 0; i < 2; i++) {
        struct marquee *m = &mq[i];
        if (!m->w || now - m->t < (m->off ? MQ_STEP : MQ_PAUSE))
            continue;
        m->off += m->off ? (int)((now - m->t) / MQ_STEP) : 1;
        m->t = now;
        if (m->off >= m->w + MQ_GAP)
            m->off = 0;                         /* back at the start: still again */
        lcd_text_scroll(MQ_X, m->y, m->s, MQ_XMAX, m->off, MQ_GAP, m->fg, BG);
    }
}

static void draw_playing(int full)
{
    char b[96], *p;
    if (!q_playing && !q_n) {
        if (full) {
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(8, 60, "nothing plays: PLAY on a file", DIM, BG);
        }
        return;
    }
    /* Each band is drawn over the previous picture, every pixel written once: no clearing first,
     * which flickered (ETUDE §39). A full drawing also paints the gaps between the bands. */
    if (full) {
        mq[0].w = mq[1].w = 0;                  /* set again below, with the new texts */
        lcd_fill(0, 16, LCD_W, 2, BG);
        lcd_fill(0, 82, LCD_W, 4, BG);
        lcd_fill(0, 118, LCD_W, 4, BG);
        lcd_fill(0, 166, LCD_W, LCD_H - 166, BG);
        lcd_fill(0, 0, 4, 16, BG);
        lcd_fill(0, 18, 4, 64, BG);             /* the margins of the title, artist, album, */
        lcd_fill(LCD_W - 4, 18, 4, 64, BG);     /* time and info lines */
        lcd_fill(0, 86, 4, 32, BG);
        lcd_fill(LCD_W - 4, 86, 4, 32, BG);
        lcd_fill(0, 122, 4, 16, BG);
        lcd_fill(0, 150, 4, 16, BG);
        lcd_fill(0, 138, LCD_W, 2, BG);         /* around the bar */
        lcd_fill(0, 140, 4, 5, BG);
        lcd_fill(LCD_W - 4, 140, 4, 5, BG);
        lcd_fill(0, 145, LCD_W, 5, BG);
    }
    /* status: playing / paused, position in the queue */
    static const char *const modes[MODE_COUNT] = { "", "  repeat all", "  repeat one", "  shuffle" };
    p = put_str(b, !q_playing ? (parked ? "paused" : "stopped") : audio_paused ? "paused" : "playing");
    p = put_str(p, "  ");
    p = put_dec(p, (uint32_t)q_cur + 1);
    p = put_str(p, "/");
    p = put_dec(p, (uint32_t)q_n);
    p = put_str(p, modes[play_mode]);
    *p = 0;
    lcd_text_clip(4, 0, b, LCD_W - 104, TITLE_FG, BG);
    draw_clock(LCD_W - 104, LCD_W - 68, full);
    p = battery_text(b);
    *p = 0;
    text_right(LCD_W - 68, LCD_W, 0, b, DIM, BG);
    if (full) {
        /* the title (or the file name) at twice the size, on two lines: as much as fits (cut
         * after a space when there is one in the second half), then the rest cut with an
         * ellipsis; then the artist and the album (or the folder) */
        const char *nm = tg.title[0] ? tg.title : q_name(q_cur), *s = nm, *s_sp = 0;
        char l1[96];
        int w = 0, n = 0, n_sp = 0;
        while (*s) {
            const char *t = s;
            uint32_t c = utf8_next(&t);
            char one[5] = { 0 };
            for (int k = 0; k < t - s && k < 4; k++)
                one[k] = s[k];
            int cw = lcd_text_width_x(one, 2);
            if (w + cw > LCD_W - 8 || n + (t - s) >= (int)sizeof l1 - 1)
                break;
            for (const char *k = s; k < t; k++)
                l1[n++] = *k;
            w += cw;
            s = t;
            if (c == ' ')
                n_sp = n, s_sp = s;
        }
        if (*s && s_sp && n_sp > n / 2)         /* a break between words */
            n = n_sp, s = s_sp;
        l1[n] = 0;
        lcd_text_clip_x(4, 18, l1, LCD_W - 4, FG, BG, 2);
        lcd_text_clip_x(4, 50, s, LCD_W - 4, FG, BG, 2);
        marquee_set(&mq[0], 86, tg.artist, TITLE_FG);
        marquee_set(&mq[1], 102, tg.album[0] ? tg.album : q.folder, DIR_FG);
    }
    if (sk_on) {                                /* the target, how far, what the keys do */
        p = put_str(b, "go to ");
        p = fmt_time(p, sk_ms);
        p = put_str(p, " / ");
        p = fmt_time(p, player_heard.length_ms);
        p = put_str(p, sk_ms >= sk_from ? "   +" : "   -");
        p = fmt_time(p, sk_ms >= sk_from ? sk_ms - sk_from : sk_from - sk_ms);
        lcd_text_clip(4, 122, b, LCD_W, SK_FG, BG);
        seek_bar(140, 5, now_ms(), sk_ms, player_heard.length_ms);
        lcd_text_clip(4, 150, "VOL+/- move  PLAY go  BACK cancel", LCD_W, DIM, BG);
        return;
    }
    p = fmt_time(b, now_ms());
    p = put_str(p, " / ");
    p = fmt_time(p, player_heard.length_ms);
    *p = 0;
    lcd_text_clip(4, 122, b, LCD_W, FG, BG);
    progress_bar(140, 5, now_ms(), player_heard.length_ms);
    p = put_str(b, codec_name(q.e[q_cur].flags & F_CODEC));
    p = put_str(p, "  ");
    p = put_dec(p, player_heard.rate / 1000);
    if (player_heard.rate % 1000) {
        *p++ = '.';
        p = put_dec(p, player_heard.rate % 1000 / 100);
    }
    p = put_str(p, " kHz  ");
    p = put_dec(p, player_heard.bps);
    p = put_str(p, " bit   -");
    p = put_dec(p, (uint32_t)play_level / 2);
    p = put_str(p, " dB  ");
    *p = 0;
    /* left to right, each pixel once: the text, the ReplayGain box (on yellow when the gain is
     * the track's), then the rest of the line */
    int x = lcd_text(4, 150, b, FG, BG);
    if (rg_mode != RG_MODE_OFF) {
        p = put_str(b, " RG ");
        if (rg_tag == RG_NONE)
            p = put_str(p, "none");
        else {
            unsigned a = (unsigned)(rg_steps < 0 ? -rg_steps : rg_steps);
            *p++ = rg_steps < 0 ? '-' : '+';
            p = put_dec(p, a / 2);
            *p++ = '.';
            *p++ = a % 2 ? '5' : '0';
        }
        p = put_str(p, " ");
        *p = 0;
        int track = rg_tag != RG_NONE && rg_src == RG_TRACK;
        x = lcd_text(x, 150, b, track ? BG : FG, track ? RG_TRACK_BG : BG);
    }
    if (x < LCD_W)
        lcd_fill(x, 150, LCD_W - x, 16, BG);
}

/* ---- main menu ---- */
enum { M_FILES, M_PLAYING, M_CLOCK, M_RG, M_USB_RO, M_USB_RW, M_INSTALL, M_TESTS, M_SYSTEM, M_LOADER,
       M_COUNT };
static const char *const m_names[M_COUNT] = {
    "Files", "Now playing", "Date and time", "ReplayGain: ", "USB disk, read only", "USB disk, read/write",
    "Install a firmware (.IMG)", "Audio tests", "System screen", "USB loader mode (exit: RST)",
};

static int main_menu(void)
{
    int sel = 0, top = 0, dirty = 1;
    for (;;) {
        if (dirty) {                            /* ROWS of the M_COUNT entries: scrolled */
            if (sel < top)
                top = sel;
            if (sel >= top + ROWS)
                top = sel - ROWS + 1;
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(4, 0, "Menu", TITLE_FG, BG);
            for (int i = top; i < M_COUNT && i < top + ROWS; i++) {
                uint16_t bg = i == sel ? SEL_BG : BG;
                int y = ROW_Y0 + 16 * (i - top);
                lcd_fill(0, y, LCD_W, 16, bg);
                char b[48];
                char *p = put_str(b, m_names[i]);
                if (i == M_RG)
                    p = put_str(p, rg_names[rg_mode]);
                *p = 0;
                lcd_text_clip(12, y, b, LCD_W, FG, bg);
            }
            dirty = 0;
        }
        sys_idle();
        ui_background();
        int k, ev = key_poll(&k);
        if (ev == EV_NONE) {
            cpu_wait();
            continue;
        }
        if (k == KEY_PLAY && ev == EV_PRESS && sel == M_RG) {
            rg_mode = (rg_mode + 1) % RG_MODES; /* album, track, off: changed in place */
            rg_apply();
            resume_save();
            dirty = 1;
        } else if (k == KEY_VOLUP && ev != EV_LONG) {
            sel = sel ? sel - 1 : M_COUNT - 1;
            dirty = 1;
        } else if (k == KEY_VOLDOWN && ev != EV_LONG) {
            sel = sel < M_COUNT - 1 ? sel + 1 : 0;
            dirty = 1;
        } else if (k == KEY_BACK || (k == KEY_MENU && ev == EV_PRESS))
            return -1;
        else if (k == KEY_PLAY && ev == EV_PRESS)
            return sel;
    }
}

/* ---- date and time: VOL+/VOL- change the field, PLAY the next one (the last one saves), BACK
 * the previous one (the first one cancels), MENU cancels. Playback goes on meanwhile. ---- */
static void clock_screen(void)
{
    struct rtc_tm t;
    int ok = rtc_get(&t), f = 0, dirty = 1;
    if (ok < 0) {
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        lcd_text(8, 60, "no clock found (I2C1, 0xA2)", WARN, BG);
        mdelay(1500);
        return;
    }
    if (!ok)
        t.year = 2026, t.mon = 1, t.day = 1, t.hour = 12, t.min = 0;
    t.sec = 0;
    for (;;) {
        if (dirty) {
            static const uint8_t ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            int dmax = ml[t.mon - 1] + (t.mon == 2 && t.year % 4 == 0);
            if (t.day > dmax)
                t.day = (uint8_t)dmax;
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(4, 0, "Date and time", TITLE_FG, BG);
            unsigned v[5] = { t.year, t.mon, t.day, t.hour, t.min };
            static const uint8_t wd[5] = { 4, 2, 2, 2, 2 };
            static const char *const sep[5] = { "-", "-", "  ", ":", "" };
            int x = 8;
            for (int i = 0; i < 5; i++) {
                char b[6];
                for (int k = wd[i] - 1, n = (int)v[i]; k >= 0; k--, n /= 10)
                    b[k] = (char)('0' + n % 10);
                b[wd[i]] = 0;
                int w = lcd_text_width_x(b, 2);
                lcd_text_clip_x(x, 50, b, x + w, FG, i == f ? SEL_BG : BG, 2);
                x += w;
                if (*sep[i]) {
                    w = lcd_text_width_x(sep[i], 2);
                    lcd_text_clip_x(x, 50, sep[i], x + w, DIM, BG, 2);
                    x += w;
                }
            }
            lcd_text(4, 118, "VOL+/VOL-: change   PLAY: next, save", DIM, BG);
            lcd_text(4, 136, "BACK: previous, cancel   MENU: cancel", DIM, BG);
            if (!ok)
                lcd_text(4, 154, "the clock had stopped: not set", WARN, BG);
            dirty = 0;
        }
        sys_idle();
        ui_background();
        int k, ev = key_poll(&k);
        if (ev == EV_NONE) {
            cpu_wait();
            continue;
        }
        if (k == KEY_VOLUP || k == KEY_VOLDOWN) {
            int d = k == KEY_VOLUP ? 1 : -1;
            if (f == 0)
                t.year = (uint16_t)(t.year + d < 2024 ? 2099 : t.year + d > 2099 ? 2024 : t.year + d);
            else if (f == 1)
                t.mon = (uint8_t)((t.mon - 1 + 12 + d) % 12 + 1);
            else if (f == 2)
                t.day = (uint8_t)((t.day - 1 + 31 + d) % 31 + 1);
            else if (f == 3)
                t.hour = (uint8_t)((t.hour + 24 + d) % 24);
            else
                t.min = (uint8_t)((t.min + 60 + d) % 60);
            dirty = 1;
        } else if (k == KEY_PLAY && ev == EV_PRESS) {
            if (++f == 5) {
                int r = rtc_set(&t);
                lcd_fill(0, 0, LCD_W, LCD_H, BG);
                lcd_text(8, 60, r ? "the clock did not answer" : "clock set", r ? WARN : FG, BG);
                mdelay(800);
                clock_min = -1;
                return;
            }
            dirty = 1;
        } else if (k == KEY_BACK && ev == EV_PRESS) {
            if (f-- == 0)
                return;
            dirty = 1;
        } else if (k == KEY_MENU && ev == EV_PRESS)
            return;
    }
}

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

static void resume_save(void)
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

static const char *find_name;
static uint32_t find_cluster;
static struct fat_dir *find_dir;
static int find_entry(const struct fat_dirent *d, void *ctx)
{
    (void)ctx;
    if ((d->attr & FAT_ATTR_DIR) && d->cluster == find_cluster) {
        copy_name((char *)find_name, d->name, 64);
        fat_dir_of(d, find_dir);
        return 1;
    }
    return 0;
}

/* path[1]: the root of a volume */
static void path_root(int vol)
{
    depth = 1;
    path[0].sel = vol == FAT_VOL_SD;
    path[0].top = 0;
    fat_root(vol, &path[1].dir);
    copy_name(path[1].name, vol == FAT_VOL_SD ? "SD card" : "Internal", sizeof path[1].name);
    path[1].sel = path[1].top = 0;
}

/* The card, set up and mounted on first use (about 40 to 220 ms) */
static int sd_open(void)
{
    if (sdvol.mounted)
        return 0;
    if (!sd_present())
        return -1;
    uint32_t t0 = ticks();
    if (sd_init() || fat_mount_sd()) {
        sd_bad = 1;
        log_printf("ui: card not readable");
        return -1;
    }
    sd_bad = 0;
    log_printf("ui: card mounted in %u ms: %s, %u MiB, %u clusters of %u sectors",
               (ticks() - t0) / 1000, sdvol.exfat ? "exFAT" : "FAT32", sd.sectors / 2048,
               sdvol.clusters, sdvol.spc);
    return 0;
}

/* At boot: back to the last folder, its selection, the track (where it was) */
static void resume_load(void)
{
    struct resume *r = (struct resume *)rs_buf;
    if (log_state_read(rs_buf) || r->sum != rs_sum(r) ||
        (__builtin_memcmp(r->magic, "NEWFWSTATE 3", 12) &&
         __builtin_memcmp(r->magic, "NEWFWSTATE 2", 12)) || (r->depth & 0xff) >= DEPTH_MAX - 1)
        return;
    /* state 2 (before 0.58): no top level, index 0 was the internal root */
    int old = r->magic[11] == '2', saved = (int)(r->depth & 0xff) + old, vol = (int)(r->depth >> 8);
    rs_last_sum = r->sum;
    if (r->level <= 0xfe)
        play_level = (int)r->level;
    if ((r->mode & 0xff) < MODE_COUNT)
        play_mode = (int)(r->mode & 0xff);
    if ((r->mode >> 8) < RG_MODES)
        rg_mode = (int)(r->mode >> 8);
    if (old)
        vol = FAT_VOL_INT;
    if (!saved || vol > FAT_VOL_SD || (vol == FAT_VOL_SD && sd_open())) {
        depth = 0;                              /* the top level (or the card is gone) */
        path[0].sel = old ? 0 : r->sel[0];
        dir_load(&path[0].dir);
        if (path[0].sel >= dir_n)
            path[0].sel = 0;
        log_printf("ui: resume: top level (%d saved)", saved);
        return;
    }
    path_root(vol);
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
               vol == FAT_VOL_SD ? "card" : "internal", depth, saved, r->track, r->pos_ms,
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

/* ---- the loop ---- */
enum { SCR_BROWSER, SCR_PLAYING };

static void enter(int i)
{
    const struct entry *e = ent(i);
    struct fat_dir d;
    if (depth + 1 >= DEPTH_MAX)
        return;
    dir_of_entry(e, &d);
    b_dirty = 1;
    if (d.vol == FAT_VOL_SD && sd_open()) {
        dir_load(&path[depth].dir);             /* "SD card (not readable)" */
        return;
    }
    depth++;
    path[depth].dir = d;
    path[depth].sel = path[depth].top = 0;
    copy_name(path[depth].name, ent_name(i), sizeof path[depth].name);
    dir_load(&d);
    resume_save();
}

static void leave(void)
{
    if (!depth)
        return;
    depth--;
    dir_load(&path[depth].dir);
    b_dirty = 1;
    resume_save();
}

/* The card-detect switch, every 250 ms, a change taken after two equal reads. Removed: what
 * plays from it stops, the browser leaves it. Returns 1 when the screen must be drawn again, 2
 * when what played stopped (the browser is shown). */
static int sd_watch(void)
{
    static int seen = -1, count;
    static uint32_t t;
    if (millis() - t < 250)
        return 0;
    t = millis();
    int in = sd_present();
    if (seen < 0 || in == seen) {
        seen = in;
        count = 0;
        return 0;
    }
    if (++count < 2)
        return 0;
    seen = in;
    count = 0;
    log_printf("ui: card %s", in ? "inserted" : "removed");
    sdvol.mounted = 0;
    sd.ready = sd_bad = 0;
    int stopped = 0;
    if (!in && q_n && (q.e[0].flags & F_SD)) {
        if (q_playing || player_is_open())
            play_stop(STOP_NOW);
        parked = 0;
        q_n = 0;
        q_cur = 0;
        stopped = 1;
    }
    if (!in && depth && path[1].dir.vol == FAT_VOL_SD)
        depth = 0;
    if (!depth)
        dir_load(&path[0].dir);
    if (path[depth].sel >= dir_n)
        path[depth].sel = path[depth].top = 0;
    b_dirty = 1;
    resume_save();
    return 1 + stopped;
}

static void move(int d)
{
    int *sel = &path[depth].sel, *top = &path[depth].top;
    if (!dir_n)
        return;
    *sel += d;
    if (*sel < 0)
        *sel = dir_n - 1;
    if (*sel >= dir_n)
        *sel = 0;
    if (*sel < *top)
        *top = *sel;
    if (*sel >= *top + ROWS)
        *top = *sel - ROWS + 1;
}

static void volume(int d)
{
    play_level += d;
    if (play_level < 0)
        play_level = 0;
    if (play_level > 0xfe)
        play_level = 0xfe;
    if (audio.on && audio.vol != 0xff && !audio_paused)
        audio_volume((uint8_t)out_level(), 0);
}

#ifndef SCREEN_TIMEOUT_US
#define SCREEN_TIMEOUT_US 30000000u          /* screen off after 30 s without a key */
#endif
#ifndef PARK_MS
#define PARK_MS     60000u                      /* a pause this long: the track is parked */
#endif
#ifndef AUTO_OFF_MS
#define AUTO_OFF_MS 600000u                     /* nothing played, no key: power off */
#endif

/* Power saving while nothing plays (ETUDE §30): parked after PARK_MS of pause; with the screen
 * off, the clocks on the crystal; AUTO_OFF_MS without a key, off (not with USB power) */
static void power_service(uint32_t key_ms)
{
    uint32_t idle = millis() - key_ms;
    if (q_playing && audio_paused && idle > PARK_MS) {
        parked_ms = player_heard.time_ms;
        play_stop(STOP_NOW);
        hifi_sleep();
        parked = 1;
        log_printf("ui: parked at %u ms after %u s of pause", parked_ms, idle / 1000);
        resume_save();
    }
    static int off_tried;                       /* still powered after an attempt: wait again */
    static uint32_t t_off;
    int quiet = !q_playing && sys_screen_is_off();
    if (quiet && !vbus() && idle > AUTO_OFF_MS && (!off_tried || millis() - t_off > AUTO_OFF_MS)) {
        sys_power_off("no key for 10 min");
        off_tried = 1;
        t_off = millis();
    }
    if (quiet && !clock_is_low && cpu_hz > 24000000 && !vbus()) {     /* not in the fallback */
        log_printf("ui: clocks on the crystal (screen off, nothing playing)");
        log_flush();
        clock_low(1);
    } else if (clock_is_low && (!quiet || vbus())) {
        clock_low(0);
        log_printf("ui: clocks back on the PLL, %u MHz", cpu_hz / 1000000);
    }
}

/* Drawing times (ETUDE §39): per screen, frames redrawn in full (a new track, a new page) and
 * partial ones (the periodic refresh, keys); logged every minute when something was drawn */
static void draw_stats(int browser, int whole, uint32_t us)
{
    static struct draw_stat { uint32_t n, sum, max; } st[2][2];
    static uint32_t t0;
    struct draw_stat *e = &st[browser][whole];
    e->n++;
    e->sum += us;
    if (us > e->max)
        e->max = us;
    if (millis() - t0 < 60000)
        return;
    t0 = millis();
    for (int b = 0; b < 2; b++) {
        if (!st[b][0].n && !st[b][1].n)
            continue;
        log_printf("ui: draw %s: %u partial, mean %u us, max %u us; %u full, mean %u us, max %u us",
                   b ? "browser" : "play screen", st[b][0].n,
                   st[b][0].n ? st[b][0].sum / st[b][0].n : 0, st[b][0].max, st[b][1].n,
                   st[b][1].n ? st[b][1].sum / st[b][1].n : 0, st[b][1].max);
    }
    __builtin_memset(st, 0, sizeof st);
}

void ui_main(void)
{
    int scr = SCR_BROWSER, full = 1, quick = 0, drawn_cur = -2, drawn_playing = -1, was_off = 0;
    uint32_t drawn_gen = 0;
    int lvl_key = play_level;                   /* the volume before a VOL key: undone by a chord */
    uint32_t t_draw = 0, t_key = ticks(), t_save = millis(), key_ms = millis();
    depth = 0;
    fat_root(VOL_TOP, &path[0].dir);
    copy_name(path[0].name, "Storage", sizeof path[0].name);
    path[0].sel = path[0].top = 0;
    resume_load();
    if (!depth && !dir_n) {                     /* nothing saved: the internal volume */
        path_root(FAT_VOL_INT);
        dir_load(&path[1].dir);
    }
    for (;;) {
        sys_idle();
        ui_background();
        audio_power_service();
        power_service(key_ms);
        int sw = sd_watch();
        if (sw)
            full = 1;
        if (sw == 2)
            scr = SCR_BROWSER;
        if (q_playing && !audio_paused && millis() - t_save > 60000) {
            t_save = millis();                  /* the position, for a resume after a cut */
            resume_save();
        }
        if (sys_screen_is_off())
            was_off = 1;
        else if (was_off) {                     /* on again (a key, or Power): a fresh delay */
            was_off = 0;
            full = b_dirty = 1;
            t_key = ticks();
        }
        if (!kp_key)
            lvl_key = play_level;
        int k = 0, ev = key_poll(&k);
        if (ev != EV_NONE) {
            t_key = ticks();
            key_ms = millis();
            if (sys_screen_is_off()) {          /* the first key only switches the screen on */
                sys_screen_set(1);
                ev = EV_NONE;
            }
        } else if (!sys_screen_is_off() && ticks() - t_key > SCREEN_TIMEOUT_US)
            sys_screen_set(0);
        if (sk_on && (millis() - sk_key_ms > SEEK_IDLE_MS || sk_cur != q_cur ||
                      (!q_playing && !parked))) {
            seek_end(1);                        /* no key for a while: go there (or the track */
            full = 1;                           /* changed meanwhile: nothing) */
        }
        if (ev == EV_CHORD) {
            if (scr == SCR_PLAYING && !sk_on) {
                play_level = lvl_key;           /* the steps of a VOL key held before it undone */
                volume(0);
                seek_begin();
                full = 1;
            }
        } else if (ev != EV_NONE && sk_on && scr == SCR_PLAYING && !(k == KEY_MENU && ev == EV_LONG)) {
            if (is_vol(k))
                seek_move(k == KEY_VOLUP ? 1 : -1, ev == EV_REPEAT);
            else if (ev == EV_PRESS) {
                seek_end(k != KEY_BACK);        /* PLAY, MENU: go there; BACK: cancel */
                full = 1;
            }
            quick = 1;
        } else if (ev != EV_NONE) {
            if (sk_on)
                seek_end(0);
            if (k == KEY_MENU && ev == EV_LONG) {
                int m = main_menu();
                if (m == M_FILES)
                    scr = SCR_BROWSER;
                else if (m == M_CLOCK)
                    clock_screen();
                else if (m == M_PLAYING)
                    scr = SCR_PLAYING;
                else if (m >= M_USB_RO) {
                    play_stop(STOP_NOW);        /* the rest needs the audio and core B */
                    if (m == M_USB_RO || m == M_USB_RW)
                        sys_usb(m == M_USB_RW);
                    else if (m == M_INSTALL)
                        sys_install();
                    else if (m == M_TESTS)
                        sys_tests();
                    else if (m == M_SYSTEM)
                        sys_screen();
                    else if (m == M_LOADER)
                        sys_loader_usb();
                    if (m == M_USB_RW || m == M_INSTALL || m == M_USB_RO) {
                        path_root(FAT_VOL_INT); /* the volume may have changed */
                        dir_load(&path[1].dir);
                        q_n = 0;
                    }
                }
                full = b_dirty = 1;
            } else if (k == KEY_MENU && ev == EV_PRESS) {
                scr = scr == SCR_BROWSER ? SCR_PLAYING : SCR_BROWSER;
                full = b_dirty = 1;
            } else if (scr == SCR_BROWSER) {
                if (k == KEY_VOLUP)
                    move(-1);
                else if (k == KEY_VOLDOWN)
                    move(1);
                else if (k == KEY_BACK && ev == EV_PRESS)
                    leave();
                else if (k == KEY_PLAY && dir_n) {
                    int sel = path[depth].sel;
                    if ((ent(sel)->flags & F_DIR) && ev == EV_PRESS)
                        enter(sel);
                    else if (ent(sel)->flags & F_DIR) {
                        char nm[64];            /* held on a folder: all that is under it */
                        copy_name(nm, ent_name(sel), sizeof nm);
                        struct fat_dir d;
                        dir_of_entry(ent(sel), &d);
                        b_dirty = 1;
                        if (d.vol != FAT_VOL_SD || !sd_open()) {
                            q_from_tree(&d, nm);
                            if (q_n) {
                                play_index(0);
                                scr = SCR_PLAYING;
                            }
                        } else
                            dir_load(&path[depth].dir);     /* "SD card (not readable)" */
                    } else if (ev == EV_PRESS) {
                        if (parked && q_cur < q_n && same_file(ent(sel), &q.e[q_cur]))
                            player_seek_hint(parked_ms);    /* the parked track: where it was */
                        q_from_dir(sel);
                        play_index(q_cur);
                        scr = SCR_PLAYING;
                    }
                }
                full = 1;
            } else {                            /* now playing */
                if (k == KEY_VOLUP)
                    volume(-2);
                else if (k == KEY_VOLDOWN)
                    volume(2);
                else if (k == KEY_PLAY && ev == EV_PRESS) {
                    if (q_playing) {
                        audio_pause(!audio_paused);
                        resume_save();
                    }
                    else if (q_n) {
                        if (parked)
                            player_seek_hint(parked_ms);
                        play_index(q_cur);
                    }
                } else if (k == KEY_PLAY && ev == EV_LONG) {
                    int n = q_next_index(q_cur, 1);
                    if (n >= 0)
                        play_index(n);
                    else if (q_n && (q_playing || parked)) {
                        play_stop(STOP_NOW);    /* past the end of the queue: stopped there, */
                        parked = 0;             /* PLAY plays the last track from its start */
                        player_heard.time_ms = 0;
                        log_printf("ui: end of the queue (next on the last track)");
                        resume_save();
                    }
                } else if (k == KEY_BACK && ev == EV_PRESS) {
                    if (q_n) {
                        if (player_heard.time_ms > 3000 || q_cur == 0)
                            play_index(q_cur);  /* back to the start of the track */
                        else
                            play_index(q_cur - 1);
                    }
                } else if (k == KEY_BACK && ev == EV_LONG) {
                    play_mode = (play_mode + 1) % MODE_COUNT;
                    rnd ^= ticks();
                    resume_save();
                }
                quick = 1;                      /* the bands redrawn, not the title: a new */
            }                                   /* track sets full below */
        }
        if (q_cur != drawn_cur || q_playing != drawn_playing || heard_gen != drawn_gen) {
            drawn_cur = q_cur;                  /* another track (the queue moved on) */
            drawn_playing = q_playing;
            drawn_gen = heard_gen;
            full = b_dirty = 1;
        }
        /* Drawing: at most one frame every 33 ms (30/s, key repeat included), the periodic
         * refresh 4/s (play screen) or 2/s, nothing while the screen is off */
        if (!sys_screen_is_off()) {
            uint32_t since = ticks() - t_draw;
            if (((full || quick) && since >= 33000) || since > (scr == SCR_PLAYING ? 250000u : 500000u)) {
                t_draw = ticks();
                int whole = scr == SCR_BROWSER ? b_dirty : full;
                if (scr == SCR_BROWSER) {
                    draw_browser();             /* b_dirty set where the page changes */
                } else
                    draw_playing(full);
                draw_stats(scr == SCR_BROWSER, whole, ticks() - t_draw);
                full = quick = 0;
            } else if (scr == SCR_PLAYING)
                marquee_tick();
        }
        cpu_wait();                             /* until the next 1 ms tick */
    }
}
