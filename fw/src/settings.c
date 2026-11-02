/* Settings (ETUDE §81): their screen (main menu), their values, and their storage, a block of
 * its own in the resume state's sector (resume.c). The ReplayGain mode stays in the resume state
 * (since 0.55); the sleep timer is not kept. */
#include "ui_priv.h"

static const struct settings defaults = {
    .screen = 1, .bright = 10, .start = START_PLAY, .auto_off = 1, .list_title = 1,
};
struct settings cfg = {
    .screen = 1, .bright = 10, .start = START_PLAY, .auto_off = 1, .list_title = 1,
};
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

/* ---- storage: 64 words, versioned settings, and a sum ---- */
struct set_block {
    char magic[12];
    struct settings s;
    uint32_t sum;
};

struct set_block_v2 {
    char magic[12];
    struct settings_v2 {
        uint8_t screen, bright, names, start, auto_off, swap_vol;
        int8_t rg_pre;
        uint8_t uac_vol;
        uint8_t language;
    } s;
    uint32_t sum;
};

struct set_block_v1 {
    char magic[12];
    struct settings_v1 {
        uint8_t screen, bright, names, start, auto_off, swap_vol;
        int8_t rg_pre;
        uint8_t uac_vol;
    } s;
    uint32_t sum;
};

static uint32_t set_sum(const void *block, unsigned words)
{
    const uint32_t *w = block;
    uint32_t s = 0x4e465343;
    for (unsigned i = 0; i < words; i++)
        s = (s << 5 | s >> 27) ^ w[i];
    return s;
}

uint32_t settings_put(uint32_t *w)
{
    struct set_block *b = (struct set_block *)w;
    __builtin_memcpy(b->magic, "NEWFWSET 3  ", 12);
    b->s = cfg;
    b->sum = set_sum(b, __builtin_offsetof(struct set_block, sum) / 4);
    return b->sum;
}

void settings_get(const uint32_t *w)
{
    const struct set_block *b = (const struct set_block *)w;
    const struct set_block_v2 *v2 = (const struct set_block_v2 *)w;
    const struct set_block_v1 *old = (const struct set_block_v1 *)w;
    int valid = 0;
    if (!__builtin_memcmp(b->magic, "NEWFWSET 3  ", 12) &&
        b->sum == set_sum(b, __builtin_offsetof(struct set_block, sum) / 4)) {
        cfg = b->s;
        valid = 1;
    } else if (!__builtin_memcmp(v2->magic, "NEWFWSET 2  ", 12) &&
               v2->sum == set_sum(v2, __builtin_offsetof(struct set_block_v2, sum) / 4)) {
        cfg.screen = v2->s.screen;
        cfg.bright = v2->s.bright;
        cfg.names = v2->s.names;
        cfg.start = v2->s.start;
        cfg.auto_off = v2->s.auto_off;
        cfg.swap_vol = v2->s.swap_vol;
        cfg.rg_pre = v2->s.rg_pre;
        cfg.uac_vol = v2->s.uac_vol;
        cfg.language = v2->s.language;
        cfg.list_title = defaults.list_title;
        valid = 1;
    } else if (!__builtin_memcmp(old->magic, "NEWFWSET 1  ", 12)) {
        uint32_t sum = set_sum(old, __builtin_offsetof(struct set_block_v1, sum) / 4);
        if (old->sum == sum) {
            valid = 1;
            cfg.screen = old->s.screen;
            cfg.bright = old->s.bright;
            cfg.names = old->s.names;
            cfg.start = old->s.start;
            cfg.auto_off = old->s.auto_off;
            cfg.swap_vol = old->s.swap_vol;
            cfg.rg_pre = old->s.rg_pre;
            cfg.uac_vol = old->s.uac_vol;
            cfg.language = defaults.language;
            cfg.list_title = defaults.list_title;
        }
    }
    if (valid) {                              /* each value checked: a bad one, its default */
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
        if (cfg.language > 2)
            cfg.language = defaults.language;
        cfg.list_title = cfg.list_title != 0;
        log_printf("ui: settings: screen %u, brightness %u %%, %s, RG preamp %d dB, start %u, "
                   "off %u, swap %u", cfg.screen, bright_pct[cfg.bright - 1], cfg.names ? "names" : "tags",
                   cfg.rg_pre, cfg.start, cfg.auto_off, cfg.swap_vol);   /* RG mode: resume.c */
    }
    lcd_brightness(bright_pct[cfg.bright - 1]); /* (ReplayGain: when a track is heard) */
}

/* ---- the screen: VOL+/VOL- the line, PLAY the next value, PLAY held the one before,
 * BACK or MENU back (saved). Each change takes effect at once. ---- */
enum { S_CLOCK, S_SCREEN, S_BRIGHT, S_NAMES, S_LIST_TITLE, S_RG, S_RG_PRE, S_START, S_SLEEP,
       S_AUTO_OFF, S_SWAP, S_LANGUAGE, S_DEFAULTS, S_COUNT };
static const char *const language_names[] = { "English", "简体中文", "日本語" };
static const char *setting_name(int i)
{
    switch (i) {
    case S_CLOCK: return ui_tr("Date and time", "日期和时间", "日時");
    case S_SCREEN: return ui_tr("Screen off after", "屏幕关闭时间", "画面消灯時間");
    case S_BRIGHT: return ui_tr("Brightness", "亮度", "明るさ");
    case S_NAMES: return ui_tr("Track title", "曲目信息显示", "曲名表示");
    case S_LIST_TITLE: return ui_tr("File list", "文件列表显示", "ファイル一覧表示");
    case S_RG: return ui_tr("ReplayGain", "回放增益", "ReplayGain");
    case S_RG_PRE: return ui_tr("ReplayGain preamp", "回放增益前置放大", "ReplayGainプリアンプ");
    case S_START: return ui_tr("At power on", "开机时", "起動時");
    case S_SLEEP: return ui_tr("Sleep timer", "睡眠定时器", "スリープタイマー");
    case S_AUTO_OFF: return ui_tr("Power off when idle", "闲置后关机", "無操作時に電源オフ");
    case S_SWAP: return ui_tr("Swap VOL+ and VOL-", "交换音量键", "音量キーを入れ替え");
    case S_LANGUAGE: return ui_tr("Language", "语言", "言語");
    default: return ui_tr("Restore the defaults (PLAY)", "恢复默认设置（按播放键）", "設定を初期化（PLAY）");
    }
}
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
    const char *const start[START_COUNT] = {
        ui_tr("resume playing", "继续播放", "再生を再開"),
        ui_tr("resume paused", "从暂停处继续", "一時停止状態で再開"),
        ui_tr("browser", "文件浏览", "ブラウザー"),
    };
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
            p = put_str(p, ui_tr("never", "永不", "なし"));
        else if (screen_s[cfg.screen] < 60) {
            p = put_dec(p, screen_s[cfg.screen]);
            p = put_str(p, ui_tr(" s", " 秒", " 秒"));
        } else
            p = put_min(p, screen_s[cfg.screen] / 60u);
        break;
    case S_BRIGHT:
        p = put_dec(p, bright_pct[cfg.bright - 1]);
        p = put_str(p, " %");
        break;
    case S_NAMES:
        p = put_str(p, cfg.names ? ui_tr("file name", "文件名", "ファイル名") :
                                 ui_tr("tags", "标签", "タグ"));
        break;
    case S_LIST_TITLE:
        p = put_str(p, cfg.list_title ? ui_tr("track title", "歌曲标题", "曲名") :
                                       ui_tr("file name", "文件名", "ファイル名"));
        break;
    case S_RG:
        p = put_str(p, rg_mode == RG_MODE_ALBUM ? ui_tr("album", "专辑", "アルバム") :
                       rg_mode == RG_MODE_TRACK ? ui_tr("track", "曲目", "トラック") :
                       rg_mode == RG_MODE_AUTO ? ui_tr("auto", "自动", "自動") :
                       ui_tr("off", "关闭", "オフ"));
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
            p = put_str(p, ui_tr("off", "关闭", "オフ"));
        else {
            p = put_min(p, sleep_left());
            p = put_str(p, ui_tr(" left", " 剩余", " 残り"));
        }
        break;
    case S_AUTO_OFF:
        p = auto_off_min[cfg.auto_off] ? put_min(p, auto_off_min[cfg.auto_off]) :
            put_str(p, ui_tr("never", "永不", "なし"));
        break;
    case S_SWAP:
        p = put_str(p, cfg.swap_vol ? ui_tr("yes", "是", "はい") : ui_tr("no", "否", "いいえ"));
        break;
    case S_LANGUAGE:
        p = put_str(p, language_names[cfg.language]);
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
    case S_LIST_TITLE:
        cfg.list_title ^= 1;
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
    case S_LANGUAGE:
        cfg.language = (uint8_t)step(cfg.language, 3, d);
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
    lcd_text_clip(12, y, setting_name(i), x, FG, bg);
    text_right(x, x1, y, b, DIR_FG, bg);
}

void settings_screen(void)
{
    int sel = 0, top = 0, dirty = 1, page = 1;
    uint32_t shown = 0;                         /* live() shown */
    for (;;) {
        if (page) {
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(4, 0, ui_tr("Settings", "设置", "設定"), TITLE_FG, BG);
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
            if (sel == S_LANGUAGE)
                page = 1;
        } else if ((k == KEY_BACK || k == KEY_MENU) && ev == EV_PRESS) {
            resume_save();
            return;
        }
    }
}
