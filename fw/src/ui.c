/* User interface (step 6, ETUDE §25): folder browser, now playing, main menu, play queue.
 * Modelled on the behaviour of RECHO's interface (no code of it): folders first, then files,
 * natural order ignoring case; VOL+/VOL- move (held: repeat), PLAY opens or plays, BACK goes up,
 * MENU switches between the browser and now playing, MENU held opens the main menu. Playback
 * runs in the background: the main loop services the player between two key polls. */
#include "ui_priv.h"

/* ---- the loop ---- */
enum { SCR_BROWSER, SCR_PLAYING };

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

/* Low battery (ETUDE §43), without USB power: the mean of the last 10 readings (one a second).
 * Under BAT_WARN_MV: a warning (banner; with music, three beeps, then one every 5 min), again
 * only once above BAT_REARM_MV. Under BAT_OFF_MV for BAT_OFF_MS: two low beeps, a pause, the
 * resume state saved, power off. Called from the loop only, never inside a write. */
#define BAT_WARN_MV   3425u           /* about 45 min left (ETUDE §65) */
#define BAT_REARM_MV  3500u
#define BAT_OFF_MV    3300u
#ifndef BAT_OFF_MS                              /* shorter in the emulator tests */
#define BAT_OFF_MS    30000u
#endif
#ifndef BAT_REMIND_MS
#define BAT_REMIND_MS 300000u
#endif
enum { BAT_OK, BAT_WARN, BAT_REDRAW };

static void battery_off(uint32_t mv)
{
    log_printf("battery: empty (%u mV for %u s)", mv, BAT_OFF_MS / 1000);
    if (q_playing && !audio_paused && audio.on) {
        audio_beep(BEEP_OFF);                   /* heard once the ring has played up to it */
        for (uint32_t t0 = millis(); millis() - t0 < 1800;) {
            sys_idle();
            ui_background();
            cpu_wait();
        }
        audio_pause(1);
    }
    sys_screen_set(1);
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    lcd_text_clip_x(8, 50, "Battery empty", LCD_W, WARN, BG, 2);
    mdelay(2000);
    sys_power_off("battery empty");             /* the resume state: paused where it was */
}                                               /* (returns when the USB cable powers us) */

static int battery_service(void)
{
    static uint32_t mv[10], t_read, t_remind, under_ms;
    static int n, warned, beep_due;
    if (millis() - t_read < 1000)
        return BAT_OK;
    t_read = millis();
    if (vbus()) {                               /* charging: the reading means nothing */
        n = warned = beep_due = 0;
        under_ms = 0;
        return BAT_OK;
    }
    for (int i = 9; i > 0; i--)
        mv[i] = mv[i - 1];
    mv[0] = battery_mv();
    if (n < 10 && ++n < 10)
        return BAT_OK;
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++)
        sum += mv[i];
    uint32_t avg = sum / 10;
    int playing = q_playing && !audio_paused && audio.on;
    int r = BAT_OK;
    if (!warned && avg < BAT_WARN_MV) {
        warned = beep_due = 1;
        log_printf("battery: low (%u mV)", avg);
        r = BAT_WARN;
    } else if (warned && avg > BAT_REARM_MV)
        warned = beep_due = 0;
    if (warned && playing && (beep_due || millis() - t_remind > BAT_REMIND_MS)) {
        audio_beep(beep_due ? BEEP_WARN : BEEP_REMIND);
        beep_due = 0;
        t_remind = millis();
    }
    if (avg >= BAT_OFF_MV)
        under_ms = 0;
    else if (!under_ms)
        under_ms = millis() ? millis() : 1;
    else if (millis() - under_ms > BAT_OFF_MS) {
        battery_off(avg);
        under_ms = 0;
        r = BAT_REDRAW;
    }
    return r;
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

/* MENU held: the main menu, then what was chosen in it */
static void menu_open(int *scr)
{
    int m = main_menu();
    if (m == M_FILES)
        *scr = SCR_BROWSER;
    else if (m == M_CLOCK)
        clock_screen();
    else if (m == M_PLAYING)
        *scr = SCR_PLAYING;
    else if (m >= M_USB_RO) {
        play_stop(STOP_NOW);                    /* the rest needs the audio and core B */
        if (m == M_USB_RO || m == M_USB_RW)
            sys_usb(m == M_USB_RW);
        else if (m == M_INSTALL)
            sys_install();
        else if (m == M_SYSTEM)
            sys_screen();
        else if (m == M_LOADER)
            sys_loader_usb();
        sd_forget();                            /* the card unwatched meanwhile: maybe changed */
        log_printf("ui: card to be set up again (after the %s)", m == M_SYSTEM ? "system screen" :
                   m == M_INSTALL ? "installer" : "USB session");
        if (m == M_USB_RW || m == M_INSTALL || m == M_USB_RO) {
            path_root(FAT_VOL_INT);             /* the volume may have changed */
            dir_load(&path[1].dir);
            q_clear();
        }
    }
}

void ui_main(void)
{
    int scr = SCR_BROWSER, full = 1, quick = 0, drawn_cur = -2, drawn_playing = -1, was_off = 0;
    uint32_t drawn_gen = 0;
    int lvl_key = play_level;                   /* the volume before a VOL key: undone by a chord */
    uint32_t t_draw = 0, t_key = ticks(), t_save = millis(), key_ms = millis();
    uint32_t banner_ms = 0;                     /* a banner is shown (low battery, cannot play) */
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
        int bat = battery_service();
        if (bat == BAT_REDRAW)
            full = b_dirty = 1;
        if (bat == BAT_WARN) {                  /* the banner, 3 s, screen on */
            banner_ms = millis() ? millis() : 1;
            sys_screen_set(1);
            t_key = ticks();
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text_clip_x(8, 50, "Low battery", LCD_W, WARN, BG, 2);
            lcd_text(8, 100, "plug the USB cable", DIM, BG);
        }
        if (fail_due) {                         /* nothing could be played: why, 3 s, then the */
            fail_due = 0;                       /* browser */
            banner_ms = millis() ? millis() : 1;
            sys_screen_set(1);
            t_key = ticks();
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            const char *what, *hint;
            fail_text(fail_code, fail_why, &what, &hint);
            lcd_text_clip_x(8, 20, "Cannot play", LCD_W, WARN, BG, 2);
            lcd_text_clip(8, 62, fail_name, LCD_W - 8, FG, BG);
            lcd_text_clip(8, 92, what, LCD_W - 8, FG, BG);
            lcd_text_clip(8, 118, hint, LCD_W - 8, DIM, BG);
            scr = SCR_BROWSER;
        }
        if (banner_ms && millis() - banner_ms > 3000) {
            banner_ms = 0;
            full = b_dirty = 1;
        }
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
        if (!key_held())
            lvl_key = play_level;
        struct key_ev e;
        int ev = key_poll(&e), k = e.key;
        if (ev == EV_PLUG || ev == EV_UNPLUG)
            ev = EV_NONE;                       /* logged (keys.c); no action yet */
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
        int r = 0;
        if (ev == EV_CHORD) {
            if (scr == SCR_PLAYING && !sk_on) {
                play_level = lvl_key;           /* the steps of a VOL key held before it undone */
                volume(0);
                seek_begin();
                r = UI_FULL;
            }
        } else if (ev != EV_NONE && sk_on && scr == SCR_PLAYING && !(k == KEY_MENU && ev == EV_LONG))
            r = seek_key(&e);
        else if (ev != EV_NONE) {
            if (sk_on)
                seek_end(0);
            if (k == KEY_MENU && ev == EV_LONG) {
                menu_open(&scr);
                r = UI_FULL;
                b_dirty = 1;
            } else if (k == KEY_MENU && ev == EV_PRESS) {
                scr = scr == SCR_BROWSER ? SCR_PLAYING : SCR_BROWSER;
                r = UI_FULL;
                b_dirty = 1;
            } else
                r = scr == SCR_BROWSER ? browser_key(&e) : play_key(&e);
        }
        if (r & UI_FULL)
            full = 1;
        if (r & UI_QUICK)
            quick = 1;
        if (r & UI_TO_PLAYING)
            scr = SCR_PLAYING;
        if (q_cur != drawn_cur || q_playing != drawn_playing || heard_gen != drawn_gen) {
            drawn_cur = q_cur;                  /* another track (the queue moved on) */
            drawn_playing = q_playing;
            drawn_gen = heard_gen;
            full = b_dirty = 1;
        }
        /* Drawing: at most one frame every 33 ms (30/s, key repeat included), the periodic
         * refresh 4/s (play screen) or 2/s, nothing while the screen is off */
        if (!sys_screen_is_off() && !banner_ms) {
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
