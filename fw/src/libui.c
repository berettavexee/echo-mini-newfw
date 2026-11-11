/* The library's update screen and status (ETUDE §125-§126): libbuild.c does the work; this
 * screen shows it and lets BACK stop it. Nothing plays meanwhile (core B's memory is the work
 * memory): the player is stopped first. Opened from the line Library of the settings, and at
 * once after a USB session in which the computer wrote. */
#include "ui_priv.h"
#include "board.h"
#include "fat.h"
#include "emmc.h"
#include "libbuild.h"
#include "library.h"
#include "storage.h"

/* The tracks of the index in use (the header's count), or -1: no index */
int lib_status(void)
{
    static uint32_t h[128] __attribute__((section(".screenram")));     /* (not the BSS) */
    struct fat_file f;
    uint32_t first, count;
    if (fat_find_root(LIB_FILE, &f) || f.size != LIB_FILE_SIZE || fat_extent(&f, &first, &count) ||
        emmc_read(first, 1, h))
        return -1;
    const uint8_t *b = (const uint8_t *)h;
    if (__builtin_memcmp(b, "NEWFWLIB", 8) || h[3] > 1)
        return -1;
    return (int)h[9];                           /* +36: the tracks of the half in use */
}

static uint32_t t_start, t_scan, last_done;
static int last_phase = -1;

static void draw_frame(void)
{
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    lcd_text_clip_x(8, 14, T("Updating the library"), LCD_W, TITLE_FG, BG, 2);
    lcd_text_clip(8, 142, T("BACK: stop (the library before is kept)"), LCD_W - 8, DIM, BG);
}

/* The screen redrawn twice a second at most (keys and the watchdog at each call), each pixel
 * written once (text drawn with its background, no clearing first: ETUDE §39), only what
 * changed; the bar only where it moved on */
static int lib_progress(int phase, uint32_t done, uint32_t total)
{
    static const char *const what[] = { N_("Looking for the music"), N_("Reading the tags"),
                                        N_("Sorting"), N_("Writing the library") };
    static uint32_t t_draw;
    static char last_cnt[24], last_left[48];
    static int bar_w;
    char b[64], *p;
    int force = 0;
    struct key_ev e;
    while (key_poll(&e))
        /* BACK pressed, or held (0.102b6 ignored it held) */
        if (e.key == KEY_BACK && (e.type == EV_PRESS || e.type == EV_LONG)) {
            log_printf("library: stopped by BACK (%s, %u of %u)", what[phase & 3], done, total);
            return 1;
        }
    if (phase != last_phase) {
        last_phase = phase;
        if (phase == LB_SCAN)
            t_scan = millis();
        lcd_fill(0, 50, LCD_W, 80, BG);
        lcd_text_clip(8, 54, tr(what[phase & 3]), LCD_W - 8, FG, BG);
        last_cnt[0] = last_left[0] = 0;
        bar_w = -1;
        force = 1;
    }
    if (!force && millis() - t_draw < 500)
        return 0;
    t_draw = millis();
    if (phase == LB_SORT || phase == LB_WRITE)  /* one bar going forward: per mille */
        p = log_format(b, b + sizeof b - 1, EN("%u %%"), done / 10);
    else
        p = log_format(b, b + sizeof b - 1, phase == LB_COUNT ? T("%u files") : T("%u of %u"),
                       done, total);
    *p = 0;
    int i = 0;
    while (b[i] && b[i] == last_cnt[i])
        i++;
    if (b[i] || last_cnt[i]) {
        text_right(8, LCD_W - 8, 78, b, FG, BG);
        copy_name(last_cnt, b, sizeof last_cnt);
    }
    if (total) {
        int w = (int)((uint64_t)(LCD_W - 8) * (done < total ? done : total) / total);
        if (bar_w < 0)
            progress_bar(100, 6, done, total);
        else if (w > bar_w)
            lcd_fill(4 + bar_w, 100, w - bar_w, 6, BAR_FG);
        if (w > bar_w)
            bar_w = w;
    }
    if (phase == LB_SCAN && done > 20 && done < total) {      /* the time left */
        uint32_t left = (uint32_t)((uint64_t)(millis() - t_scan) * (total - done) / done / 1000);
        p = left >= 120 ? log_format(b, b + sizeof b - 1, T("about %u min left"), (left + 30) / 60)
                        : log_format(b, b + sizeof b - 1, T("about %u s left"), left);
        *p = 0;
        i = 0;
        while (b[i] && b[i] == last_left[i])
            i++;
        if (b[i] || last_left[i]) {
            lcd_text_clip(8, 112, b, LCD_W - 8, DIM, BG);   /* (it fills the rest of the line) */
            copy_name(last_left, b, sizeof last_left);
        }
    }
    last_done = done;
    return 0;
}

static const struct lb_hooks hooks = { lib_progress };

int lib_update(void)
{
    storage_drain();
    play_stop(STOP_NOW);                        /* core B's memory is the work memory */
    libv_close();                               /* the index changes: shown again from it */
    if (sd_present())
        sd_open();                              /* (a card in, not mounted yet: its files) */
    sys_screen_set(1);
    draw_frame();
    t_start = millis();
    last_phase = -1;
    struct lb_result r;
    int e = lib_build(&hooks, &r);
    lcd_fill(0, 50, LCD_W, LCD_H - 50, BG);
    char b[64], *p;
    if (!e)
        p = log_format(b, b + sizeof b - 1, T("Library updated: %u tracks"), r.tracks);
    else if (e == LB_ECANCEL)
        p = log_format(b, b + sizeof b - 1, "%s", T("Stopped: the library before is kept"));
    else if (e == LB_EFILE)
        p = log_format(b, b + sizeof b - 1, "%s", T("No room for NEWFWLIB.DAT"));
    else if (e == LB_ETRACKS || e == LB_ENAMES || e == LB_EFULL || e == LB_EDIRS)
        p = log_format(b, b + sizeof b - 1, "%s", T("Too much music for the library"));
    else
        p = log_format(b, b + sizeof b - 1, T("The library could not be updated (%d)"), e);
    *p = 0;
    lcd_text_clip(8, 64, b, LCD_W - 8, e ? WARN : FG, BG);
    lcd_settle();
    for (uint32_t t0 = millis(); millis() - t0 < 2000;) {
        wdt_kick();
        cpu_wait();
    }
    return e;
}
