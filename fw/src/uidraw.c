/* Drawing helpers shared by the screens: times, the progress bar, right-aligned text, the clock */
#include "ui_priv.h"

/* ---- drawing ---- */
static void put2(char **p, uint32_t v)
{
    *(*p)++ = (char)('0' + v / 10 % 10);
    *(*p)++ = (char)('0' + v % 10);
}

char *fmt_time(char *p, uint32_t ms)
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

int bar_x(uint32_t pos, uint32_t len)
{
    return len ? (int)((uint64_t)(pos > len ? len : pos) * (LCD_W - 8) / len) : 0;
}

void progress_bar(int y, int h, uint32_t pos, uint32_t len)
{
    int w = bar_x(pos, len);
    lcd_fill(4, y, w, h, BAR_FG);
    lcd_fill(4 + w, y, LCD_W - 8 - w, h, RGB(40, 40, 50));
}

/* Text right-aligned at x1 in [x0, x1) of a line: the band left of it filled with bg, each pixel
 * written once (no clearing first: it would flicker, ETUDE §39) */
void text_right(int x0, int x1, int y, const char *s, uint16_t fg, uint16_t bg)
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
int clock_min = -1;

void draw_clock(int x0, int x1, int force)
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
