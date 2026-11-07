/* User interface (step 6, ETUDE §25): folder browser, now playing, main menu, play queue.
 * Modelled on the behaviour of RECHO's interface (no code of it): folders first, then files,
 * natural order ignoring case; VOL+/VOL- move (held: repeat), PLAY opens or plays, BACK goes up,
 * MENU switches between the browser and now playing, MENU held opens the main menu. Playback
 * runs in the background: the main loop services the player between two key polls. */
#include "crash.h"
#include "ui_priv.h"
#include "storage.h"

#define STORAGE_BUDGET_US 4000                  /* the storage task per pass of the loop */

/* ---- the loop ---- */
enum { SCR_BROWSER, SCR_PLAYING };

#ifndef PARK_MS
#define PARK_MS     60000u                      /* a pause this long: the track is parked */
#endif

/* Power saving while nothing plays (ETUDE §30): parked after PARK_MS of pause; with the screen
 * off, the clocks on the crystal; nothing played and no key for auto_off_ms() (the settings), off
 * (not with USB power) */
static void power_service(uint32_t key_ms)
{
    uint32_t idle = millis() - key_ms;
    if (q_playing && audio_paused && idle > PARK_MS) {
        parked_ms = player_heard.time_ms;
        parked_len = player_heard.length_ms;
        play_stop(STOP_NOW);
        hifi_sleep();
        parked = 1;
        log_printf("ui: parked at %u ms after %u s of pause", parked_ms, idle / 1000);
        resume_save();
    }
    static int off_tried;                       /* still powered after an attempt: wait again */
    static uint32_t t_off;
    int quiet = !q_playing && sys_screen_is_off();
    uint32_t off_ms = auto_off_ms();
    if (off_ms && !q_playing && !vbus() && idle > off_ms &&
        (!off_tried || millis() - t_off > off_ms)) {
        sys_power_off("no key, nothing played");
        off_tried = 1;
        t_off = millis();
    }
    if (quiet && !clock_is_low && cpu_hz > 24000000 && !vbus()) {     /* not in the fallback */
        log_printf("ui: clocks on the crystal (screen off, nothing playing)");
        log_flush();
        clock_low(1);
    } else if (clock_is_low && (!quiet || vbus())) {
        clock_low(0);
        log_printf("ui: clocks back on the PLL, core %u Hz against the crystal (%u expected)",
                   clock_measure(50), cpu_hz);
    }
}

/* The sleep timer (settings) ended: the music faded out over 5 s, paused, power off (paused
 * where it was at the next start) */
/* The fade is a step of the main loop each pass (since 0.99r7, ETUDE §99), the keys ignored
 * meanwhile as before; 1 while it runs */
static struct { int on; uint32_t t0, v0, v; } fade;

static int sleep_service(void)
{
    if (fade.on) {
        uint32_t t = millis() - fade.t0 < 5000 ? millis() - fade.t0 : 5000;
        uint32_t want = fade.v0 + (0xfe - fade.v0) * t / 5000;
        if (want > fade.v)
            audio_volume((uint8_t)(fade.v = want), 0);
        if (fade.v < 0xfe && q_playing)
            return 1;
        fade.on = 0;
        audio_pause(1);
        rg_apply();                             /* the volume put back for PLAY */
        resume_save();
        sys_power_off("sleep timer");           /* (returns when the USB cable powers us) */
        return 0;
    }
    if (!sleep_at || (int32_t)(millis() - sleep_at) < 0)
        return 0;
    sleep_cancel();
    log_printf("ui: sleep timer ended");
    if (q_playing && !audio_paused && audio.on && audio.vol != 0xff) {
        fade.on = 1;
        fade.t0 = millis();
        fade.v0 = fade.v = audio.vol;
        return 1;
    }
    sys_power_off("sleep timer");               /* (returns when the USB cable powers us) */
    return 0;
}

/* The battery shown (ETUDE §82): the mean of the last 64 readings, one a second (about a minute:
 * a track starting, card reads or the backlight do not show), started again from the latest
 * reading when the USB power comes or goes; then a percent that moves freely one way only, down
 * on battery and up on USB, and the other way by 10 points or more; while charging, never 100 %
 * (the charger's current lifts the voltage near full). */
#define GAUGE_N 64
static uint16_t gauge_mv[GAUGE_N];
static uint32_t gauge_sum;
static int gauge_i, gauge_usb = -1, gauge_shown = -1;

static void gauge_add(uint32_t mv)
{
    int usb = vbus();
    if (usb != gauge_usb) {
        for (int i = 0; i < GAUGE_N; i++)
            gauge_mv[i] = (uint16_t)mv;
        gauge_sum = mv * GAUGE_N;
        gauge_usb = usb;
    } else {
        gauge_sum = gauge_sum - gauge_mv[gauge_i] + mv;
        gauge_mv[gauge_i] = (uint16_t)mv;
        gauge_i = (gauge_i + 1) % GAUGE_N;
    }
    int pct = (int)battery_pct(gauge_sum / GAUGE_N);
    if (usb && charging() && pct > 99)
        pct = 99;
    if (gauge_shown < 0 || (usb ? pct > gauge_shown || pct <= gauge_shown - 10 :
                                  pct < gauge_shown || pct >= gauge_shown + 10))
        gauge_shown = pct;
}

unsigned battery_avg_mv(void)
{
    if (gauge_shown < 0)
        gauge_add(battery_mv());
    return gauge_sum / GAUGE_N;
}

unsigned battery_shown(void)
{
    if (gauge_shown < 0)
        gauge_add(battery_mv());
    return (unsigned)gauge_shown;
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
    lcd_text_clip_x(8, 50, T("Battery empty"), LCD_W, WARN, BG, 2);
    lcd_settle();                               /* lit before the wait, not after it */
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
    uint32_t now_mv = battery_mv();
    gauge_add(now_mv);
    if (vbus()) {                               /* charging: the reading means nothing */
        n = warned = beep_due = 0;
        under_ms = 0;
        return BAT_OK;
    }
    for (int i = 9; i > 0; i--)
        mv[i] = mv[i - 1];
    mv[0] = now_mv;
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

/* ---- the stack of screens (ui_priv.h): above the browser and the play screen ---- */
#define SCR_DEPTH 4
static const struct screen *scr_stack[SCR_DEPTH];
static int scr_n;

void scr_push(const struct screen *s)
{
    if (scr_n == SCR_DEPTH) {
        log_printf("ui: screen stack full");
        return;
    }
    scr_stack[scr_n++] = s;
    s->enter();
}

/* What the main menu chose, once it is off the stack */
static void menu_action(int m, int *scr)
{
    if (m < 0)
        return;
    if (m == M_FILES)
        *scr = SCR_BROWSER;
    else if (m == M_SETTINGS)
        scr_push(&settings_screen);
    else if (m == M_PLAYING)
        *scr = SCR_PLAYING;
    else if (m >= M_USB_RO) {
        resume_flush();                         /* before the USB session, an install, a reboot */
        storage_drain();
        play_stop(STOP_NOW);                    /* the rest needs the audio and core B */
        if (m == M_USB_RO || m == M_USB_RW)
            sys_usb(m == M_USB_RW);
        else if (m == M_USB_DAC)
            uac_mode();
        else if (m == M_INSTALL)
            sys_install();
        else if (m == M_SYSTEM)
            sys_screen();
        else if (m == M_LOADER)
            sys_loader_usb();
        sd_forget();                            /* the card unwatched meanwhile: maybe changed */
        log_printf("ui: card to be set up again (after the %s)", m == M_SYSTEM ? "system screen" :
                   m == M_INSTALL ? "installer" : "USB session");
        if (sd_present())
            sd_open();                          /* at once: the music stopped (§106) */
        if (m == M_USB_RW || m == M_INSTALL || m == M_USB_RO) {
            path_root(FAT_VOL_INT);             /* the volume may have changed */
            dir_load(&path[1].dir);
            q_clear();
        }
    }
}

/* The top screen closed: the one below it drawn again, or the browser and the play screen (after
 * the main menu, what it chose); UI_FULL when the home screens show again */
static int scr_pop(int *scr)
{
    const struct screen *top = scr_stack[--scr_n];
    if (top == &menu_screen) {
        int m = menu_choice;
        menu_choice = -1;
        menu_action(m, scr);
    }
    if (scr_n) {
        scr_stack[scr_n - 1]->shown();
        return 0;
    }
    b_dirty = 1;
    return UI_FULL;
}

void ui_main(void)
{
    int scr = SCR_BROWSER, full = 1, quick = 0, drawn_cur = -2, drawn_playing = -1, was_off = 0;
    uint32_t drawn_gen = 0;
    int lvl_key = play_level;                   /* the volume before a VOL key: undone by a chord */
    uint32_t t_draw = 0, t_key = ticks(), t_save = millis(), key_ms = millis();
    uint32_t banner_ms = 0;                     /* a banner is shown (low battery, cannot play) */
    tags_yield = storage_yield;                 /* a tag read in the storage task gives way */
    sd_yield = storage_yield;                   /* so does a card mounted there */
    depth = 0;
    fat_root(VOL_TOP, &path[0].dir);
    path[0].sel = path[0].top = 0;
    if (sd_present())
        sd_open();                              /* the card mounted at once (0.99r26, ETUDE §106): */
    resume_load();                              /* nothing plays yet; the language set */
    copy_name(path[0].name, T("Storage"), sizeof path[0].name);
    if (!depth && !dir_n) {                     /* nothing saved: the internal volume */
        path_root(FAT_VOL_INT);
        dir_load(&path[1].dir);
    }
    if (q_playing || parked)
        scr = SCR_PLAYING;                      /* show a track restored at power-on */
    for (;;) {
        sys_idle();
        const char *pp = perf_begin("player");  /* the loop's tasks, named for perf (§97, §99) */
        ui_background();
        audio_power_service();
        perf_end(pp);
        pp = perf_begin("power");
        power_service(key_ms);
        int fading = sleep_service();
        perf_end(pp);
        pp = perf_begin("battery");
        int bat = battery_service();
        perf_end(pp);
        if (bat == BAT_REDRAW)
            full = b_dirty = 1;
        if (bat == BAT_WARN) {                  /* the banner, 3 s, screen on */
            banner_ms = millis() ? millis() : 1;
            sys_screen_set(1);
            t_key = ticks();
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text_clip_x(8, 50, T("Low battery"), LCD_W, WARN, BG, 2);
            lcd_text(8, 100, T("plug the USB cable"), DIM, BG);
        }
        if (fail_due) {                         /* nothing could be played: why, 3 s, then the */
            fail_due = 0;                       /* browser */
            banner_ms = millis() ? millis() : 1;
            sys_screen_set(1);
            t_key = ticks();
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            const char *what, *hint;
            fail_text(fail_code, fail_why, &what, &hint);
            lcd_text_clip_x(8, 20, T("Cannot play"), LCD_W, WARN, BG, 2);
            lcd_text_clip(8, 62, fail_name, LCD_W - 8, FG, BG);
            lcd_text_clip(8, 92, tr(what), LCD_W - 8, FG, BG);
            lcd_text_clip(8, 118, tr(hint), LCD_W - 8, DIM, BG);
            scr = SCR_BROWSER;
        }
        if (crash_due) {                        /* the last session ended on a fault (0.99r32): */
            crash_due = 0;                      /* once, 3 s */
            banner_ms = millis() ? millis() : 1;
            sys_screen_set(1);
            t_key = ticks();
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text_clip_x(8, 20, T("Fault"), LCD_W, WARN, BG, 2);
            lcd_text_clip(8, 62, T("The last session ended on a fault"), LCD_W - 8, FG, BG);
            lcd_text_clip(8, 88, crash_text[0], LCD_W - 8, DIM, BG);
            lcd_text_clip(8, 108, crash_text[1], LCD_W - 8, DIM, BG);
            lcd_text_clip(8, 134, T("details: NEWFWLOG.TXT"), LCD_W - 8, DIM, BG);
        }
        if (banner_ms && millis() - banner_ms > 3000) {
            banner_ms = 0;
            full = b_dirty = 1;
            if (scr_n)
                scr_stack[scr_n - 1]->shown();
        }
        pp = perf_begin("card watch");
        int sw = sd_watch();
        perf_end(pp);
        if (sw)
            full = 1;
        if (sw == 2)
            scr = SCR_BROWSER;
        if (q_playing && !audio_paused && millis() - t_save > 60000) {
            t_save = millis();                  /* the position, for a resume after a cut */
            resume_save();                      /* (its own phase, "resume save") */
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
        /* the last headphones out while playing: a pause, as PLAY (§119); no play again when
         * they come back (PLAY does it), the screen left as it is; the USB cable: nothing */
        if (ev == EV_UNPLUG && (k == SRC_JACK35 || k == SRC_JACK44) && !fading &&
            !plug_present(SRC_JACK35) && !plug_present(SRC_JACK44) && q_playing &&
            !audio_paused) {
            audio_pause(1);
            resume_save_later();
            log_printf("ui: headphones out: paused");
            if (scr == SCR_PLAYING && !sys_screen_is_off())
                full = 1;
        }
        if (ev == EV_PLUG || ev == EV_UNPLUG || fading)
            ev = EV_NONE;                       /* logged (keys.c); the fade: no key either */
        int wr = browser_walk_poll();           /* a tree walk ended: its queue plays */
        if (wr) {
            full = b_dirty = 1;
            if (wr & UI_TO_PLAYING)
                scr = SCR_PLAYING;
        }
        if (browser_walking() && ev != EV_NONE && !sys_screen_is_off() &&
            !(scr == SCR_PLAYING && !scr_n && (key_is_vol(k) || (k == KEY_PLAY && ev == EV_PRESS))))
            ev = EV_NONE;                       /* no folder read meanwhile (§100): keys wait; */
                                                /* the play screen's volume, pause, seek: on */
        if (ev != EV_NONE) {
            t_key = ticks();
            key_ms = millis();
            if (sys_screen_is_off()) {          /* the first key only switches the screen on */
                sys_screen_set(1);
                ev = EV_NONE;
            }
        } else if (!scr_n && !sys_screen_is_off() && ticks() - t_key > screen_timeout_us())
            sys_screen_set(0);                  /* (not over a screen of the stack, as before) */
        if (sk_on && (millis() - sk_key_ms > SEEK_IDLE_MS || sk_cur != q_cur ||
                      (!q_playing && !parked))) {
            seek_end(1);                        /* no key for a while: go there (or the track */
            full = 1;                           /* changed meanwhile: nothing) */
        }
        int r = 0;
        if (scr_n) {                            /* a screen of the stack: its keys */
            if (ev != EV_NONE && ev != EV_CHORD && scr_stack[scr_n - 1]->key(&e) == SCR_POP)
                r = scr_pop(&scr);
        } else if (ev == EV_CHORD) {
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
            if (k == KEY_MENU && ev == EV_LONG)
                scr_push(&menu_screen);         /* drawn by the loop, acted on when closed */
            else if (k == KEY_MENU && ev == EV_PRESS) {
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
        if (scr_n && !sys_screen_is_off() && !banner_ms) {
            if (scr_stack[scr_n - 1]->draw() == SCR_POP && (scr_pop(&scr) & UI_FULL))
                full = b_dirty = 1;
        } else if (!sys_screen_is_off() && !banner_ms) {
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
            else
                browser_marquee_tick();
        }
        resume_flush();                         /* a save asked meanwhile, after the frame */
        pp = perf_begin("storage");
        storage_run(STORAGE_BUDGET_US);         /* the long reads, a few ms each pass (§100) */
        perf_end(pp);
        cpu_wait();                             /* until the next 1 ms tick */
    }
}
