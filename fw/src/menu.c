/* The main menu (MENU held) and the date and time screen */
#include "ui_priv.h"

/* ---- main menu ---- */
static const char *const m_names[M_COUNT] = {
    N_("Files"), N_("Now playing"), N_("Settings"), N_("USB disk, read only"),
    N_("USB disk, read/write"), N_("USB sound card"), N_("Install a firmware (.IMG)"),
    N_("System screen"), N_("USB loader mode (exit: RST)"),
};

/* A screen of the stack (ui.c, ETUDE §99): VOL+/VOL- move, PLAY chooses, BACK or MENU closes; the
 * choice in menu_choice (-1: none), acted on by ui.c once the menu is off the stack */
int menu_choice = -1;
static int m_sel, m_top, m_dirty;

static void menu_enter(void)
{
    m_sel = m_top = 0;
    m_dirty = 1;
    menu_choice = -1;
}

static void menu_shown(void)
{
    m_dirty = 1;
}

static int menu_draw(void)
{
    if (!m_dirty)                               /* ROWS of the M_COUNT entries: scrolled */
        return SCR_STAY;
    const char *pp = perf_begin("menu draw");
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    lcd_text(4, 0, T("Menu"), TITLE_FG, BG);
    for (int i = m_top; i < M_COUNT && i < m_top + ROWS; i++) {
        uint16_t bg = i == m_sel ? SEL_BG : BG;
        int y = ROW_Y0 + 16 * (i - m_top);
        lcd_fill(0, y, LCD_W, 16, bg);
        lcd_text_clip(12, y, tr(m_names[i]), list_text_end(M_COUNT, ROWS), FG, bg);
    }
    list_scrollbar(ROW_Y0, ROWS, m_top, M_COUNT);
    m_dirty = 0;
    perf_end(pp);
    return SCR_STAY;
}

static int menu_key(const struct key_ev *e)
{
    int ev = e->type, k = e->key;
    if (k == KEY_VOLUP && ev != EV_LONG) {
        list_move(&m_sel, &m_top, M_COUNT, ROWS, -1);
        m_dirty = 1;
    } else if (k == KEY_VOLDOWN && ev != EV_LONG) {
        list_move(&m_sel, &m_top, M_COUNT, ROWS, 1);
        m_dirty = 1;
    } else if (k == KEY_BACK || (k == KEY_MENU && ev == EV_PRESS)) {
        menu_choice = -1;
        return SCR_POP;
    } else if (k == KEY_PLAY && ev == EV_PRESS) {
        menu_choice = m_sel;
        return SCR_POP;
    }
    return SCR_STAY;
}

const struct screen menu_screen = { menu_enter, menu_shown, menu_key, menu_draw };

/* ---- date and time: VOL+/VOL- change the field, PLAY the next one (the last one saves), BACK
 * the previous one (the first one cancels), MENU cancels. Playback goes on meanwhile. ---- */
static struct rtc_tm c_t;
static int c_ok, c_f, c_dirty;
static uint32_t c_msg_at, c_msg_ms;             /* a message shown (millis), for c_msg_ms, then off */
static const char *c_msg;
static uint16_t c_msg_fg;

static void clock_msg(const char *m, uint16_t fg, uint32_t ms)
{
    c_msg = m;
    c_msg_fg = fg;
    c_msg_ms = ms;
    c_msg_at = millis() | 1;
    c_dirty = 1;
}

static void clock_enter(void)
{
    c_ok = rtc_get(&c_t);
    c_f = 0;
    c_dirty = 1;
    c_msg_at = 0;
    if (c_ok < 0) {
        clock_msg(T("no clock found (I2C1, 0xA2)"), WARN, 1500);
        return;
    }
    if (!c_ok)
        c_t.year = 2026, c_t.mon = 1, c_t.day = 1, c_t.hour = 12, c_t.min = 0;
    c_t.sec = 0;
}

static void clock_shown(void)
{
    c_dirty = 1;
}

static int clock_draw(void)
{
    if (c_msg_at) {                             /* the message, then the screen closes */
        if (c_dirty) {
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(8, 60, c_msg, c_msg_fg, BG);
            c_dirty = 0;
        }
        return millis() - c_msg_at >= c_msg_ms ? SCR_POP : SCR_STAY;
    }
    if (!c_dirty)
        return SCR_STAY;
    struct rtc_tm t = c_t;
    int f = c_f, ok = c_ok;
    static const uint8_t ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dmax = ml[t.mon - 1] + (t.mon == 2 && t.year % 4 == 0);
    if (t.day > dmax)
        t.day = c_t.day = (uint8_t)dmax;
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    lcd_text(4, 0, T("Date and time"), TITLE_FG, BG);
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
    lcd_text(4, 118, T("VOL+/VOL-: change   PLAY: next, save"), DIM, BG);
    lcd_text(4, 136, T("BACK: previous, cancel   MENU: cancel"), DIM, BG);
    if (!ok)
        lcd_text(4, 154, T("the clock had stopped: not set"), WARN, BG);
    c_dirty = 0;
    return SCR_STAY;
}

static int clock_key(const struct key_ev *e)
{
    int ev = e->type, k = e->key;
    if (c_msg_at)
        return SCR_STAY;                        /* the message stays its time */
    struct rtc_tm *t = &c_t;
    if (k == KEY_VOLUP || k == KEY_VOLDOWN) {
        int d = k == KEY_VOLUP ? 1 : -1;
        if (c_f == 0)
            t->year = (uint16_t)(t->year + d < 2024 ? 2099 : t->year + d > 2099 ? 2024 : t->year + d);
        else if (c_f == 1)
            t->mon = (uint8_t)((t->mon - 1 + 12 + d) % 12 + 1);
        else if (c_f == 2)
            t->day = (uint8_t)((t->day - 1 + 31 + d) % 31 + 1);
        else if (c_f == 3)
            t->hour = (uint8_t)((t->hour + 24 + d) % 24);
        else
            t->min = (uint8_t)((t->min + 60 + d) % 60);
        c_dirty = 1;
    } else if (k == KEY_PLAY && ev == EV_PRESS) {
        if (++c_f == 5) {
            int r = rtc_set(t);
            clock_msg(r ? T("the clock did not answer") : T("clock set"), r ? WARN : FG, 800);
            clock_min = -1;
            return SCR_STAY;
        }
        c_dirty = 1;
    } else if (k == KEY_BACK && ev == EV_PRESS) {
        if (c_f-- == 0)
            return SCR_POP;
        c_dirty = 1;
    } else if (k == KEY_MENU && ev == EV_PRESS)
        return SCR_POP;
    return SCR_STAY;
}

const struct screen clock_screen = { clock_enter, clock_shown, clock_key, clock_draw };
