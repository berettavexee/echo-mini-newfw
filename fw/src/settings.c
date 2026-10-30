/* Settings (ETUDE §81): their screen (main menu), their values, and their storage, a block of
 * its own in the resume state's sector (resume.c). The ReplayGain mode stays in the resume state
 * (since 0.55); the sleep timer is not kept. */
#include "ui_priv.h"

static const struct settings defaults = { 1, 10, 0, START_PLAY, 1, 0, 0, 0 };
struct settings cfg = { 1, 10, 0, START_PLAY, 1, 0, 0, 0 };
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

/* ---- storage: 64 words, "NEWFWSET 1", the values, a sum ---- */
struct set_block {
    char magic[12];
    struct settings s;
    uint32_t sum;
};

static uint32_t set_sum(const struct set_block *b)
{
    const uint32_t *w = (const uint32_t *)b;
    uint32_t s = 0x4e465343;
    for (unsigned i = 0; i < __builtin_offsetof(struct set_block, sum) / 4; i++)
        s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

uint32_t settings_put(uint32_t *w)
{
    struct set_block *b = (struct set_block *)w;
    __builtin_memcpy(b->magic, "NEWFWSET 1  ", 12);
    b->s = cfg;
    b->sum = set_sum(b);
    return b->sum;
}

void settings_get(const uint32_t *w)
{
    const struct set_block *b = (const struct set_block *)w;
    if (!__builtin_memcmp(b->magic, "NEWFWSET 1  ", 12) && b->sum == set_sum(b)) {
        cfg = b->s;                             /* each value checked: a bad one, its default */
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
        log_printf("ui: settings: screen %u, brightness %u %%, %s, RG preamp %d dB, start %u, "
                   "off %u, swap %u", cfg.screen, bright_pct[cfg.bright - 1], cfg.names ? "names" : "tags",
                   cfg.rg_pre, cfg.start, cfg.auto_off, cfg.swap_vol);   /* RG mode: resume.c */
    }
    lcd_brightness(bright_pct[cfg.bright - 1]); /* (ReplayGain: when a track is heard) */
}

/* ---- the screen: VOL+/VOL- the line, PLAY the next value, PLAY held the one before,
 * BACK or MENU back (saved). Each change takes effect at once. ---- */
enum { S_CLOCK, S_SCREEN, S_BRIGHT, S_NAMES, S_RG, S_RG_PRE, S_START, S_SLEEP, S_AUTO_OFF, S_SWAP,
       S_DEFAULTS, S_COUNT };
static const char *const s_names[S_COUNT] = {
    "Date and time", "Screen off after", "Brightness", "Track title", "ReplayGain", "ReplayGain preamp",
    "At power on", "Sleep timer", "Power off when idle", "Swap VOL+ and VOL-",
    "Restore the defaults (PLAY)",
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

static char *put_min(char *p, uint32_t m)
{
    p = put_dec(p, m);
    return put_str(p, " min");
}

static void value_text(int i, char *b)
{
    static const char *const start[START_COUNT] = { "resume playing", "resume paused", "browser" };
    char *p = b;
    switch (i) {
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
            p = put_str(p, ok < 0 ? "no clock" : "not set");
        break;
    }
    case S_SCREEN:
        if (!screen_s[cfg.screen])
            p = put_str(p, "never");
        else if (screen_s[cfg.screen] < 60) {
            p = put_dec(p, screen_s[cfg.screen]);
            p = put_str(p, " s");
        } else
            p = put_min(p, screen_s[cfg.screen] / 60u);
        break;
    case S_BRIGHT:
        p = put_dec(p, bright_pct[cfg.bright - 1]);
        p = put_str(p, " %");
        break;
    case S_NAMES:
        p = put_str(p, cfg.names ? "file name" : "tags");
        break;
    case S_RG:
        p = put_str(p, rg_names[rg_mode]);
        break;
    case S_RG_PRE:
        *p++ = cfg.rg_pre < 0 ? '-' : '+';
        p = put_dec(p, (uint32_t)(cfg.rg_pre < 0 ? -cfg.rg_pre : cfg.rg_pre));
        p = put_str(p, " dB");
        break;
    case S_START:
        p = put_str(p, start[cfg.start]);
        break;
    case S_SLEEP:
        if (!sleep_at)
            p = put_str(p, "off");
        else {
            p = put_min(p, sleep_left());
            p = put_str(p, " left");
        }
        break;
    case S_AUTO_OFF:
        p = auto_off_min[cfg.auto_off] ? put_min(p, auto_off_min[cfg.auto_off]) :
            put_str(p, "never");
        break;
    case S_SWAP:
        p = put_str(p, cfg.swap_vol ? "yes" : "no");
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
        sleep_at = sleep_min[sleep_sel] ? millis() + sleep_min[sleep_sel] * 60000u : 0;
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
    case S_DEFAULTS:
        cfg = defaults;
        rg_mode = RG_MODE_ALBUM;
        sleep_cancel();
        lcd_brightness(bright_pct[cfg.bright - 1]);
        rg_apply();
        log_printf("ui: settings: the defaults");
        break;
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
    char b[32];
    value_text(i, b);
    int x = x1 - lcd_text_width(b) - 8;
    lcd_fill(0, y, 12, 16, bg);
    lcd_text_clip(12, y, s_names[i], x, FG, bg);
    text_right(x, x1, y, b, DIR_FG, bg);
}

void settings_screen(void)
{
    int sel = 0, top = 0, dirty = 1, page = 1;
    uint32_t shown = 0;                         /* live() shown */
    for (;;) {
        if (page) {
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(4, 0, "Settings", TITLE_FG, BG);
            page = 0;
            dirty = 1;
        }
        if (dirty || live() != shown) {
            char b[16];
            char *p = put_dec(b, (uint32_t)sel + 1);
            p = put_str(p, "/");
            p = put_dec(p, S_COUNT);
            *p = 0;
            text_right(LCD_W - 70, LCD_W - 4, 0, b, DIM, BG);
            for (int i = top; i < S_COUNT && i < top + ROWS; i++)
                draw_row(i, top, sel);
            list_scrollbar(ROW_Y0, ROWS, top, S_COUNT);
            shown = live();
            dirty = 0;
        }
        sys_idle();
        ui_background();
        struct key_ev e;
        int ev = key_poll(&e), k = e.key;
        if (ev == EV_NONE) {
            cpu_wait();
            continue;
        }
        if (k == KEY_VOLUP && ev != EV_LONG) {
            int t = top;
            list_move(&sel, &top, S_COUNT, ROWS, -1);
            dirty = 1;
            page = t != top;
        } else if (k == KEY_VOLDOWN && ev != EV_LONG) {
            int t = top;
            list_move(&sel, &top, S_COUNT, ROWS, 1);
            dirty = 1;
            page = t != top;
        } else if (k == KEY_PLAY && ev == EV_PRESS && sel == S_CLOCK) {
            clock_screen();
            page = 1;
        } else if (k == KEY_PLAY && sel != S_CLOCK &&
                   (ev == EV_PRESS || (ev == EV_LONG && sel != S_DEFAULTS))) {
            change(sel, ev == EV_PRESS ? 1 : -1);
            dirty = 1;
        } else if ((k == KEY_BACK || k == KEY_MENU) && ev == EV_PRESS) {
            resume_save();
            return;
        }
    }
}
