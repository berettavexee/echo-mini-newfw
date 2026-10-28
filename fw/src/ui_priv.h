/* Shared state and functions of the user interface modules (ETUDE §25, §42):
 *   ui.c         the main loop: keys to the screen shown, drawing, power saving
 *   browser.c    top level, folder listings, path, microSD card, browser screen and keys
 *   queue.c      play queue and play modes
 *   playback.c   tracks from the queue: open, chain, skip, stop, park, volume, ReplayGain, seek
 *   playscreen.c the play screen and its keys
 *   menu.c       main menu, date and time
 *   resume.c     resume state
 *   uidraw.c     drawing helpers */
#ifndef UI_PRIV_H
#define UI_PRIV_H

#include <stdint.h>
#include "audio.h"
#include "board.h"
#include "fat.h"
#include "hifi.h"
#include "hifi_proto.h"
#include "keys.h"
#include "lcd.h"
#include "log.h"
#include "player.h"
#include "rtc.h"
#include "sd.h"
#include "sys.h"
#include "tags.h"
#include "ui.h"

#define TITLE_FG RGB(255, 210, 90)
#define DIR_FG   RGB(140, 200, 255)
#define BAR_FG   RGB(90, 160, 255)
#define SK_FG    RGB(255, 150, 60)              /* the seek target */
#define RG_TRACK_BG RGB(240, 200, 0)            /* ReplayGain of the track (not the album) */

/* What a key handler asks of the main loop */
#define UI_FULL       1                         /* draw the screen in full */
#define UI_QUICK      2                         /* redraw its bands soon */
#define UI_TO_PLAYING 4                         /* show the play screen */

/* ---- browser.c: listing entries (also the queue's) ---- */
#define DIR_MAX   1000
#define F_DIR     0x8000
#define F_SD      0x4000                        /* on the card (else the internal volume) */
#define F_CONTIG  0x2000                        /* exFAT "no FAT chain" */
#define F_VOLROOT 0x1000                        /* the top level: a volume */
#define F_CODEC   0x0fff
struct entry {
    uint32_t cluster, size;
    uint16_t name;              /* offset in the pool */
    uint16_t flags;             /* F_DIR, or the codec; F_SD, F_CONTIG, F_VOLROOT */
};
struct dir_buf {
    struct entry e[DIR_MAX];
    uint16_t order[DIR_MAX];
    char pool[81920];
};
extern int dir_n;
const char *ent_name(int i);                    /* the i-th entry shown, sorted */
const struct entry *ent(int i);
int dir_load(const struct fat_dir *d);
void dir_of_entry(const struct entry *e, struct fat_dir *d);

#define VOL_TOP   2                             /* fat_dir.vol of the top level */
#define DEPTH_MAX 16
struct path_level {
    struct fat_dir dir;         /* path[0]: the top level (VOL_TOP), path[1]: a volume's root */
    int sel, top;
    char name[64];
};
extern struct path_level path[DEPTH_MAX];
extern int depth;                               /* path[depth] is the folder shown */
void copy_name(char *d, const char *s, int max);
void path_root(int v);                          /* path[1]: the root of a volume */
int sd_open(void);                              /* the card set up and mounted, 0: ready */
void sd_forget(void);                           /* set up again at the next access */
int sd_watch(void);                             /* the detect switch: 1 redraw, 2 playback stopped */

#define ROWS   9
#define ROW_Y0 18
extern int b_dirty;                             /* the browser page must be drawn again */
void draw_browser(void);
int browser_key(const struct key_ev *e);

/* ---- queue.c ---- */
#define Q_MAX 400
struct queue {
    struct entry e[Q_MAX];
    char pool[20480];
    char folder[64];
};
extern struct queue q;
extern int q_n, q_cur, q_playing, q_errors;
extern int q_dec;                               /* the track core B decodes; q_cur: the one heard */
enum { MODE_ORDER, MODE_REPEAT_ALL, MODE_REPEAT_ONE, MODE_SHUFFLE, MODE_COUNT };
extern int play_mode;
extern uint32_t rnd;
int q_next_index(int i, int user);              /* the track after i, or -1: end of the queue */
void q_from_dir(int start);
void q_from_tree(const struct fat_dir *top, const char *name);
const char *q_name(int i);

/* ---- playback.c ---- */
extern struct tags tg;                          /* of the track heard */
extern uint32_t heard_gen;                      /* goes up when the track heard changes */
enum { RG_MODE_ALBUM, RG_MODE_TRACK, RG_MODE_OFF, RG_MODES };
extern const char *const rg_names[RG_MODES];
extern int rg_mode, rg_steps, rg_tag, rg_src;
void rg_apply(void);
extern int parked;
extern uint32_t parked_ms;
enum { STOP_NOW, STOP_SKIP, STOP_DRAIN };
void play_stop(int how);
void play_index(int i);
void play_step(int i, int step);
#define Q_EEMPTY (-60)                          /* an empty file (0 bytes) */
extern char fail_name[64];                      /* nothing could be played: the banner */
extern int fail_code, fail_due;
extern uint32_t fail_why;
void play_failed(const char *name, int code);
/* What went wrong and what to do, for the "Cannot play" screen (code: a player error) */
void fail_text(int code, uint32_t why, const char **what, const char **hint);
void volume(int d);
#ifndef SEEK_IDLE_MS
#define SEEK_IDLE_MS 2000u
#endif
extern int sk_on, sk_cur;
extern uint32_t sk_ms, sk_from, sk_key_ms;
uint32_t now_ms(void);                          /* the position shown */
void seek_begin(void);
void seek_move(int d, int repeat);
void seek_end(int go);

/* ---- playscreen.c ---- */
void draw_playing(int full);
void marquee_tick(void);
int play_key(const struct key_ev *e);
int seek_key(const struct key_ev *e);

/* ---- menu.c ---- */
enum { M_FILES, M_PLAYING, M_CLOCK, M_RG, M_USB_RO, M_USB_RW, M_INSTALL, M_SYSTEM, M_LOADER,
       M_COUNT };
int main_menu(void);
void clock_screen(void);

/* ---- resume.c ---- */
void resume_save(void);
void resume_load(void);

/* ---- uidraw.c ---- */
extern int clock_min;
char *fmt_time(char *p, uint32_t ms);
int bar_x(uint32_t pos, uint32_t len);
void progress_bar(int y, int h, uint32_t pos, uint32_t len);
void text_right(int x0, int x1, int y, const char *s, uint16_t fg, uint16_t bg);
void draw_clock(int x0, int x1, int force);

#endif
