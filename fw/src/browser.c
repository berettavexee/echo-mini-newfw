/* Browser (ETUDE §25, §38): the top level (internal memory, microSD card), folder listings
 * sorted folders first then files in natural order, the path from the top, the card set up on
 * first use and followed by its detect switch, the browser screen and its keys. */
#include "ui_priv.h"

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
#define NAME_KEEP 72                            /* bytes of a name kept (the screen shows ~40) */
struct dir_buf dir __attribute__((section(".dirbuf")));
int dir_n, dir_full;
static uint32_t dir_pool_used;

const char *ent_name(int i)
{
    return dir.pool + dir.e[dir.order[i]].name;
}

const struct entry *ent(int i)
{
    return &dir.e[dir.order[i]];
}

/* An entry at the end of the listing, its name (len bytes) into the pool; 0 when it is full */
static int dir_add(const char *name, uint32_t len, uint32_t cluster, uint32_t size, uint16_t flags)
{
    if (dir_pool_used + len + 1 > sizeof dir.pool) {
        dir_full = 1;
        return 0;
    }
    struct entry *e = &dir.e[dir_n];
    e->cluster = cluster;
    e->size = size;
    e->name = (uint16_t)dir_pool_used;
    e->flags = flags;
    for (uint32_t i = 0; i < len; i++)
        dir.pool[dir_pool_used + i] = name[i];
    dir.pool[dir_pool_used + len] = 0;
    dir_pool_used += len + 1;
    dir.order[dir_n] = (uint16_t)dir_n;
    dir_n++;
    return 1;
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
    if (!dir_add(d->name, len, d->cluster, d->size,
                 (uint16_t)((d->attr & FAT_ATTR_DIR ? F_DIR : codec) |
                            (d->vol == FAT_VOL_SD ? F_SD : 0) | (d->contig ? F_CONTIG : 0))))
        return 0;
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
int sd_bad;                                     /* the card in did not mount */

static void top_entry(const char *name, uint16_t flags)
{
    uint32_t len = 0;
    while (name[len])
        len++;
    dir_add(name, len, 0, 0, (uint16_t)(F_DIR | F_VOLROOT | flags));
}

int dir_load(const struct fat_dir *d)
{
    uint32_t t0 = ticks();
    dir_n = dir_full = 0;
    dir_pool_used = 0;
    if (d->vol == VOL_TOP) {
        top_entry("Internal", 0);
        if (sd_present())
            top_entry(sd_bad ? "SD card (not readable)" : "SD card", F_SD);
        return 0;
    }
    if (d->vol == FAT_VOL_SD && sd_open())
        return -1;
    int r = fat_list(d, collect, 0);
    if (r < 0 && d->vol == FAT_VOL_SD) {        /* a read error: mounted again at the next */
        log_printf("ui: card read failed");     /* access, "not readable" meanwhile */
        sd_forget();
        sd_bad = 1;
    }
    uint32_t t1 = ticks();
    dir_sort();
    log_printf("ui: folder %s%x: %d entries%s, read %u ms, sorted %u ms",
               d->vol == FAT_VOL_SD ? "sd:" : "", d->cluster, dir_n,
               dir_full ? " (more not shown)" : "", (t1 - t0) / 1000, (ticks() - t1) / 1000);
    return r;
}

/* The directory an entry of the listing designates */
void dir_of_entry(const struct entry *e, struct fat_dir *d)
{
    d->vol = e->flags & F_SD ? FAT_VOL_SD : FAT_VOL_INT;
    d->contig = (e->flags & F_CONTIG) != 0;
    d->cluster = e->flags & F_VOLROOT ? 0 : e->cluster;
    d->size = e->flags & F_VOLROOT ? 0 : e->size;
}

int same_file(const struct entry *a, const struct entry *b)
{
    return a->cluster == b->cluster && (a->flags & F_SD) == (b->flags & F_SD);
}

/* ---- browser state: the path from the root ---- */
struct path_level path[DEPTH_MAX];
int depth;                                      /* path[depth] is the folder shown */

void copy_name(char *d, const char *s, int max)
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

int b_dirty = 1;
static int b_sel = -1, b_top = -1, b_mark = -1;

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
    lcd_text_clip(22, y, ent_name(i), LCD_W, dirent ? DIR_FG : e->size ? FG : DIM, bg);
}

void draw_browser(void)
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

/* path[1]: the root of a volume */
void path_root(int vol)
{
    depth = 1;
    path[0].sel = vol == FAT_VOL_SD;
    path[0].top = 0;
    fat_root(vol, &path[1].dir);
    copy_name(path[1].name, vol == FAT_VOL_SD ? "SD card" : "Internal", sizeof path[1].name);
    path[1].sel = path[1].top = 0;
}

/* The card, set up and mounted on first use (about 40 to 220 ms) */
/* The card set up again at the next access: after a time without the detect switch watched
 * (USB, installer, system screens), the card in may be another one, or taken out and back */
void sd_forget(void)
{
    sdvol.mounted = 0;
    sd.ready = 0;
}

int sd_open(void)
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

/* A folder of the card could not be read: the top level, "SD card (not readable)" */
static void card_failed(void)
{
    if (!sd_bad || !depth || path[depth].dir.vol != FAT_VOL_SD)
        return;
    depth = 0;
    path[0].sel = 1;
    path[0].top = 0;
    dir_load(&path[0].dir);
}

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
    card_failed();
    resume_save();
}

static void leave(void)
{
    if (!depth)
        return;
    depth--;
    dir_load(&path[depth].dir);
    card_failed();
    b_dirty = 1;
    resume_save();
}

/* The card-detect switch, every 250 ms, a change taken after two equal reads. Removed: what
 * plays from it stops, the browser leaves it. Returns 1 when the screen must be drawn again, 2
 * when what played stopped (the browser is shown). */
int sd_watch(void)
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
    sd_forget();
    sd_bad = 0;
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

/* A key on the browser: VOL+/VOL- move, BACK goes up, PLAY opens a folder or plays the file and
 * those after it, PLAY held on a folder plays all that is under it. Returns UI_* (ui_priv.h). */
int browser_key(const struct key_ev *e)
{
    int k = e->key, ev = e->type;
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
            char nm[64];                        /* held on a folder: all that is under it */
            copy_name(nm, ent_name(sel), sizeof nm);
            struct fat_dir d;
            dir_of_entry(ent(sel), &d);
            b_dirty = 1;
            if (d.vol != FAT_VOL_SD || !sd_open()) {
                q_from_tree(&d, nm);
                if (q_n) {
                    play_index(0);
                    return UI_FULL | UI_TO_PLAYING;
                }
            } else
                dir_load(&path[depth].dir);     /* "SD card (not readable)" */
        } else if (ev == EV_PRESS && !ent(sel)->size) {
            play_failed(ent_name(sel), Q_EEMPTY); /* empty (grey): not played, the queue kept */
        } else if (ev == EV_PRESS) {
            if (parked && q_cur < q_n && same_file(ent(sel), &q.e[q_cur]))
                player_seek_hint(parked_ms);    /* the parked track: where it was */
            q_from_dir(sel);
            play_index(q_cur);
            return UI_FULL | UI_TO_PLAYING;
        }
    }
    return UI_FULL;
}
