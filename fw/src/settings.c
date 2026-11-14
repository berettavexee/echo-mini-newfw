/* Settings (ETUDE §81): their screen (main menu), their values, and their storage, a block of
 * its own in the resume state's sector (resume.c). The ReplayGain mode stays in the resume state
 * (since 0.55); the sleep timer is not kept. */
#include "ui_priv.h"
#include "lang.h"

static const struct settings defaults = { 1, 10, 0, START_PLAY, 1, 0, 0, 0, 1, "", 0, BAT_ICON };
struct settings cfg = { 1, 10, 0, START_PLAY, 1, 0, 0, 0, 1, "", 0, BAT_ICON };
uint32_t sleep_at;

static const uint16_t screen_s[] = { 10, 30, 60, 120, 300, 0 };     /* 0: never */
static const uint8_t auto_off_min[] = { 5, 10, 20, 30, 0 };
static const uint8_t sleep_min[] = { 0, 15, 30, 45, 60, 90 };       /* 0: off */
/* The backlight of each step, %: about x1.3 a step, even steps to the eye (EchoPod's curve) */
static const uint8_t bright_pct[10] = { 8, 12, 17, 23, 30, 39, 50, 63, 80, 100 };
#define N(a) (int)(sizeof(a) / sizeof(a)[0])
static int sleep_sel;                                               /* in sleep_min */

uint32_t screen_timeout_us(void)
{
    uint32_t s = screen_s[cfg.screen];
    return s ? s * 1000000u : 0xffffffffu;
}

uint32_t auto_off_ms(void)
{
    return auto_off_min[cfg.auto_off] * 60000u;
}

void sleep_cancel(void)
{
    sleep_at = 0;
    sleep_sel = 0;
}

static const char *const bat_log[BAT_COUNT] = { "icon", "percent", "time left" };

/* ---- storage: 64 words, "NEWFWSET 3", the values, a sum. Version 2 (0.99 to 0.100) had the
 * values up to uac_remote, its sum in word 6; version 1 (up to 0.99) the first 8 bytes, its sum
 * in word 5: read, the others at their defaults ---- */
struct set_block {
    char magic[12];
    struct settings s;
    uint32_t sum;
};
#define SET_V1_WORDS 5
#define SET_V2_WORDS 6
_Static_assert(sizeof(struct set_block) <= 32 * 4, "settings reach the free space (resume.c)");
/* hide_clock (§120) took the padding byte after lang: the sum stays in word 8, so that a block
 * of 0.101 reads as it is (the clock shown) and 0.101 reads ours */
/* bat_show took the next padding byte, the same way: a block of 0.104 reads with the icon */
_Static_assert(__builtin_offsetof(struct set_block, sum) == 32, "version 3's sum in word 8");

static uint32_t set_sum(const uint32_t *w, unsigned n)
{
    uint32_t s = 0x4e465343;
    for (unsigned i = 0; i < n; i++)
        s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

uint32_t settings_put(uint32_t *w)
{
    struct set_block *b = (struct set_block *)w;
    __builtin_memset(b, 0, sizeof *b);          /* the padding too: it is in the sum */
    __builtin_memcpy(b->magic, "NEWFWSET 3  ", 12);
    b->s = cfg;
    b->sum = set_sum(w, __builtin_offsetof(struct set_block, sum) / 4);
    return b->sum;
}

void settings_get(const uint32_t *w)
{
    const struct set_block *b = (const struct set_block *)w;
    int v3 = !__builtin_memcmp(b->magic, "NEWFWSET 3  ", 12) &&
             b->sum == set_sum(w, __builtin_offsetof(struct set_block, sum) / 4);
    int v2 = !__builtin_memcmp(b->magic, "NEWFWSET 2  ", 12) &&
             w[SET_V2_WORDS] == set_sum(w, SET_V2_WORDS);
    int v1 = !__builtin_memcmp(b->magic, "NEWFWSET 1  ", 12) &&
             w[SET_V1_WORDS] == set_sum(w, SET_V1_WORDS);
    if (v1 || v2 || v3) {
        cfg = defaults;                         /* each value checked: a bad one, its default */
        __builtin_memcpy(&cfg, &b->s, v3 ? sizeof cfg : v2 ? SET_V2_BYTES : 8);
        cfg.lang[sizeof cfg.lang - 1] = 0;
        if (cfg.screen >= N(screen_s))
            cfg.screen = defaults.screen;
        if (cfg.bright < 1 || cfg.bright > 10)
            cfg.bright = defaults.bright;
        cfg.names = cfg.names != 0;
        if (cfg.start >= START_COUNT)
            cfg.start = defaults.start;
        if (cfg.auto_off >= N(auto_off_min))
            cfg.auto_off = defaults.auto_off;
        cfg.swap_vol = cfg.swap_vol != 0;
        if (cfg.rg_pre < -6 || cfg.rg_pre > 6)
            cfg.rg_pre = 0;
        cfg.uac_remote = cfg.uac_remote != 0;
        cfg.hide_clock = cfg.hide_clock != 0;
        if (cfg.bat_show >= BAT_COUNT)
            cfg.bat_show = defaults.bat_show;
        log_printf("ui: settings %u: screen %u, brightness %u %%, %s, RG preamp %d dB, start %u, "
                   "off %u, swap %u, remote %u, language %s, clock %s, battery %s",
                   v3 ? 3 : v2 ? 2 : 1,
                   cfg.screen, bright_pct[cfg.bright - 1], cfg.names ? "names" : "tags",
                   cfg.rg_pre, cfg.start, cfg.auto_off, cfg.swap_vol, cfg.uac_remote,
                   cfg.lang[0] ? cfg.lang : "en", cfg.hide_clock ? "hidden" : "shown",
                   bat_log[cfg.bat_show]);
    }                                           /* (RG mode: resume.c) */
    lang_set_code(cfg.lang);                    /* unknown to this image: English */
    lcd_brightness(bright_pct[cfg.bright - 1]); /* (ReplayGain: when a track is heard) */
}

/* ---- the screen: VOL+/VOL- the line, PLAY the next value, PLAY held the one before,
 * BACK or MENU back (saved). Each change takes effect at once. ---- */
enum { S_LANG, S_CLOCK, S_SHOW_CLOCK, S_BATTERY, S_SCREEN, S_BRIGHT, S_NAMES, S_RG, S_RG_PRE,
       S_START, S_SLEEP, S_AUTO_OFF, S_SWAP, S_LIBRARY, S_DEFAULTS, S_COUNT };
static const char *const s_names[S_COUNT] = {
    EN("Language"),                             /* in English: found again in any language */
    N_("Date and time"),
    /* TRANSLATORS: the time shown at the top of the play screen and the browser */
    N_("Clock"),
    /* TRANSLATORS: the battery at the top right: its icon, its percent or the music left */
    N_("Battery"), N_("Screen off after"), N_("Brightness"), N_("Track title"),
    N_("ReplayGain"), N_("ReplayGain preamp"), N_("At power on"), N_("Sleep timer"),
    N_("Power off when idle"), N_("Swap VOL+ and VOL-"),
    /* TRANSLATORS: the music library (artists, albums...): its index, PLAY updates it */
    N_("Library (PLAY: update)"), N_("Restore the defaults (PLAY)"),
};
static const uint8_t rg_order[RG_MODES] = { RG_MODE_ALBUM, RG_MODE_TRACK, RG_MODE_AUTO,
                                            RG_MODE_OFF };

static char *put_2(char *p, unsigned v)
{
    *p++ = (char)('0' + v / 10 % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

static uint32_t sleep_left(void)                /* minutes, rounded up */
{
    int32_t left = (int32_t)(sleep_at - millis());
    return sleep_at && left > 0 ? ((uint32_t)left + 59999) / 60000 : 0;
}

static int lib_tracks = -1;                     /* lib_status(), read on entering */

/* The value of line i, into b[VALUE_MAX] */
#define VALUE_MAX 48
static void value_text(int i, char *b)
{
    static const char *const start[START_COUNT] = { N_("resume playing"), N_("resume paused"),
                                                    N_("browser") };
    char *p = b, *end = b + VALUE_MAX - 1;
    switch (i) {
    case S_LANG:                                /* its own name (po: the translation of */
        p = log_format(p, end, "%s", lang_name(lang_current()));   /* N_("English")) */
        break;
    case S_CLOCK: {                             /* PLAY: its screen (menu.c) */
        struct rtc_tm t;
        int ok = rtc_get(&t);
        if (ok == 1) {
            p = put_dec(p, t.year);
            *p++ = '-';
            p = put_2(p, t.mon);
            *p++ = '-';
            p = put_2(p, t.day);
            *p++ = ' ';
            p = put_2(p, t.hour);
            *p++ = ':';
            p = put_2(p, t.min);
        } else
            p = log_format(p, end, "%s", ok < 0 ? T("no clock") : T("not set"));
        break;
    }
    case S_SHOW_CLOCK:
        /* TRANSLATORS: the value of the line "Clock": the time shown or not */
        p = log_format(p, end, "%s", cfg.hide_clock ? T("hidden") : T("shown"));
        break;
    case S_BATTERY: {
        /* TRANSLATORS: the values of the line "Battery" */
        static const char *const v[BAT_COUNT] = { N_("icon"), N_("percent"), N_("time left") };
        p = log_format(p, end, "%s", tr(v[cfg.bat_show]));
        break;
    }
    case S_SCREEN:
        if (!screen_s[cfg.screen])
            p = log_format(p, end, "%s", T("never"));
        else if (screen_s[cfg.screen] < 60)
            /* TRANSLATORS: seconds */
            p = log_format(p, end, T("%u s"), screen_s[cfg.screen]);
        else
            /* TRANSLATORS: minutes */
            p = log_format(p, end, T("%u min"), screen_s[cfg.screen] / 60u);
        break;
    case S_BRIGHT:
        p = log_format(p, end, EN("%u %%"), bright_pct[cfg.bright - 1]);
        break;
    case S_NAMES:
        p = log_format(p, end, "%s", cfg.names ? T("file name") : T("tags"));
        break;
    case S_RG:
        p = log_format(p, end, "%s", tr(rg_names[rg_mode]));
        break;
    case S_RG_PRE:
        p = log_format(p, end, EN("%c%u dB"), cfg.rg_pre < 0 ? '-' : '+',
                       (uint32_t)(cfg.rg_pre < 0 ? -cfg.rg_pre : cfg.rg_pre));
        break;
    case S_START:
        p = log_format(p, end, "%s", tr(start[cfg.start]));
        break;
    case S_SLEEP:
        if (!sleep_at)
            p = log_format(p, end, "%s", T("off"));
        else
            /* TRANSLATORS: the minutes left before the sleep timer stops the music */
            p = log_format(p, end, T("%u min left"), sleep_left());
        break;
    case S_AUTO_OFF:
        p = auto_off_min[cfg.auto_off] ?
            log_format(p, end, T("%u min"), auto_off_min[cfg.auto_off]) :
            log_format(p, end, "%s", T("never"));
        break;
    case S_SWAP:
        p = log_format(p, end, "%s", cfg.swap_vol ? T("yes") : T("no"));
        break;
    case S_LIBRARY:
        if (lib_tracks < 0)
            p = log_format(p, end, "%s", T("none"));
        else
            p = log_format(p, end, T("%u tracks"), (uint32_t)lib_tracks);
        break;
    }
    *p = 0;
}

static int step(int v, int n, int d)            /* the next or previous of n values, around */
{
    return (v + n + d) % n;
}

static void change(int i, int d)
{
    switch (i) {
    case S_LANG: {
        int n = lang_count(), l = step(lang_current(), n, d);
        lang_set(l);
        l = lang_current();                     /* (English if its catalogue failed) */
        const char *c = l ? lang_code(l) : "";
        unsigned k = 0;
        for (; k < sizeof cfg.lang - 1 && c[k]; k++)
            cfg.lang[k] = c[k];
        while (k < sizeof cfg.lang)
            cfg.lang[k++] = 0;
        browser_lang_changed();
        log_printf("ui: language %s", l ? lang_code(l) : "en");
        break;
    }
    case S_SHOW_CLOCK:
        cfg.hide_clock ^= 1;
        clock_min = -1;                         /* drawn again (or cleared) at once */
        log_printf("ui: clock %s", cfg.hide_clock ? "hidden" : "shown");
        break;
    case S_BATTERY:
        cfg.bat_show = (uint8_t)step(cfg.bat_show, BAT_COUNT, d);
        log_printf("ui: battery %s", bat_log[cfg.bat_show]);
        break;
    case S_SCREEN:
        cfg.screen = (uint8_t)step(cfg.screen, N(screen_s), d);
        break;
    case S_BRIGHT:
        cfg.bright = (uint8_t)(step(cfg.bright - 1, 10, d) + 1);
        lcd_brightness(bright_pct[cfg.bright - 1]);
        break;
    case S_NAMES:
        cfg.names ^= 1;
        break;
    case S_RG: {
        int k = 0;
        while (k < RG_MODES - 1 && rg_order[k] != rg_mode)
            k++;
        rg_mode = rg_order[step(k, RG_MODES, d)];
        rg_apply();
        break;
    }
    case S_RG_PRE:
        cfg.rg_pre = (int8_t)(step(cfg.rg_pre + 6, 13, d) - 6);
        rg_apply();
        break;
    case S_START:
        cfg.start = (uint8_t)step(cfg.start, START_COUNT, d);
        break;
    case S_SLEEP:
        sleep_sel = step(sleep_sel, N(sleep_min), d);
#ifdef ECHO_SLEEP_SECONDS
        /* test build (the emulator's scenario sleep-fade): the minutes counted as seconds */
        sleep_at = sleep_min[sleep_sel] ? millis() + sleep_min[sleep_sel] * 1000u : 0;
#else
        sleep_at = sleep_min[sleep_sel] ? millis() + sleep_min[sleep_sel] * 60000u : 0;
#endif
        if (sleep_at)
            sleep_at |= 1;                      /* never 0 when on */
        log_printf("ui: sleep timer %u min", sleep_min[sleep_sel]);
        break;
    case S_AUTO_OFF:
        cfg.auto_off = (uint8_t)step(cfg.auto_off, N(auto_off_min), d);
        break;
    case S_SWAP:
        cfg.swap_vol ^= 1;
        break;
    case S_DEFAULTS: {
        char keep[sizeof cfg.lang];             /* the language stays: the defaults are read */
        __builtin_memcpy(keep, cfg.lang, sizeof keep);  /* in it */
        cfg = defaults;
        __builtin_memcpy(cfg.lang, keep, sizeof keep);
        rg_mode = RG_MODE_ALBUM;
        sleep_cancel();
        lcd_brightness(bright_pct[cfg.bright - 1]);
        rg_apply();
        clock_min = -1;                         /* the clock shown again */
        log_printf("ui: settings: the defaults");
        break;
    }
    }
}

/* What changes on its own on the screen: the clock's minute, the sleep timer's minutes */
static uint32_t live(void)
{
    struct rtc_tm t;
    uint32_t m = rtc_get(&t) == 1 ? t.hour * 60u + t.min + 1 : 0;
    return m << 16 | sleep_left();
}

static void draw_row(int i, int top, int sel)
{
    int y = ROW_Y0 + 16 * (i - top), x1 = list_text_end(S_COUNT, ROWS);
    uint16_t bg = i == sel ? SEL_BG : BG;
    char b[VALUE_MAX];
    value_text(i, b);
    int x = x1 - lcd_text_width(b) - 8;
    lcd_fill(0, y, 12, 16, bg);
    lcd_text_clip(12, y, i == S_LANG ? s_names[i] : tr(s_names[i]), x, FG, bg);
    text_right(x, x1, y, b, DIR_FG, bg);
}

/* A screen of the stack (ui.c, ETUDE §99) */
static int s_sel, s_top, s_dirty, s_page;
static uint32_t s_shown;                        /* live() shown */

static void settings_enter(void)
{
    s_sel = s_top = 0;
    s_dirty = s_page = 1;
    s_shown = 0;
    lib_tracks = lib_status();
}

static void settings_shown(void)
{
    s_page = 1;
}

static int settings_draw(void)
{
    const char *pp = perf_begin("settings draw");
    if (s_page) {                               /* no clear first: each band once (§147) */
        head_title(T("Settings"), TITLE_FG, "");
        lcd_fill(0, ROW_Y0 + 16 * ROWS, LCD_W, LCD_H - ROW_Y0 - 16 * ROWS, BG);
        head_status(1);
        s_page = 0;
        s_dirty = 1;
    } else
        head_status(0);
    static uint32_t t_live, now_live;           /* the clock read 4 times a second, not each */
    if (s_dirty || millis() - t_live >= 250) {  /* pass (an I2C read, about 1.9 ms) */
        t_live = millis();
        now_live = live();
    }
    if (s_dirty || now_live != s_shown) {
        char b[16];
        char *p = put_dec(b, (uint32_t)s_sel + 1);
        p = put_str(p, "/");
        p = put_dec(p, S_COUNT);
        *p = 0;
        head_extra(b);
        for (int i = s_top; i < S_COUNT && i < s_top + ROWS; i++)
            draw_row(i, s_top, s_sel);
        list_scrollbar(ROW_Y0, ROWS, s_top, S_COUNT);
        s_shown = now_live;
        s_dirty = 0;
    }
    perf_end(pp);
    return SCR_STAY;
}

static int settings_key(const struct key_ev *e)
{
    int ev = e->type, k = e->key;
    if (k == KEY_VOLUP && ev != EV_LONG) {     /* (scrolled: the rows only, each drawn whole; */
        list_move(&s_sel, &s_top, S_COUNT, ROWS, -1);   /* the header again flickered: §149) */
        s_dirty = 1;
    } else if (k == KEY_VOLDOWN && ev != EV_LONG) {
        list_move(&s_sel, &s_top, S_COUNT, ROWS, 1);
        s_dirty = 1;
    } else if (k == KEY_PLAY && ev == EV_PRESS && s_sel == S_CLOCK) {
        scr_push(&clock_screen);
    } else if (k == KEY_PLAY && ev == EV_PRESS && s_sel == S_LIBRARY) {
        resume_save_later();
        lib_update();                           /* (its own screen: the music stops) */
        lib_tracks = lib_status();
        s_dirty = s_page = 1;
    } else if (k == KEY_PLAY && s_sel != S_CLOCK && s_sel != S_LIBRARY &&
               (ev == EV_PRESS || (ev == EV_LONG && s_sel != S_DEFAULTS))) {
        change(s_sel, ev == EV_PRESS ? 1 : -1);
        s_dirty = 1;
        s_page |= s_sel == S_LANG;              /* the title too, in the new language */
    } else if ((k == KEY_BACK || k == KEY_MENU) && ev == EV_PRESS) {
        resume_save_later();
        return SCR_POP;
    }
    return SCR_STAY;
}

const struct screen settings_screen = { settings_enter, settings_shown, settings_key,
                                        settings_draw };
