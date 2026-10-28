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

/* ---- lists ---- */
#define SB_W   3                                /* the scroll bar, at the right edge */
#define SB_GAP 3                                /* between the text and the bar */

void list_move(int *sel, int *top, int n, int rows, int d)
{
    if (n <= 0)
        return;
    *sel += d;
    if (*sel < 0)
        *sel = n - 1;
    if (*sel >= n)
        *sel = 0;
    if (*sel < *top)
        *top = *sel;
    if (*sel >= *top + rows)
        *top = *sel - rows + 1;
}

int list_text_end(int n, int rows)
{
    return n > rows ? LCD_W - SB_W - SB_GAP : LCD_W;
}

/* The bar beside rows of 16 px from y: the thumb's height is the share of the list shown (8 px
 * at least), its place that of the first row shown; nothing when the list fits */
void list_scrollbar(int y, int rows, int top, int n)
{
    if (n <= rows)
        return;
    int h = rows * 16, t = h * rows / n, room;
    if (t < 8)
        t = 8;
    room = h - t;
    int ty = (int)((int64_t)room * top / (n - rows));
    /* the gap before it: rows drawn full width before (a list that fitted) leave their colour */
    lcd_fill(LCD_W - SB_W - SB_GAP, y, SB_GAP, h, BG);
    lcd_fill(LCD_W - SB_W, y, SB_W, ty, RGB(40, 40, 50));
    lcd_fill(LCD_W - SB_W, y + ty, SB_W, t, BAR_FG);
    lcd_fill(LCD_W - SB_W, y + ty + t, SB_W, h - ty - t, RGB(40, 40, 50));
}

