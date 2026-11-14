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

/* "HH:MM" right-aligned at x1 in [x0, x1) of the first line, once the clock has been set and
 * unless hidden (Settings, §120: the place left blank); redrawn when the minute changes or when
 * forced */
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
    if (!ok || cfg.hide_clock)
        b[0] = 0;
    text_right(x0, x1, 0, b, DIM, BG);
}

/* ---- the header (ETUDE §147): on the first 16 rows, the screen's title at the left; at the right,
 * an extra text (a list's "n/N"), the clock, what plays (play, pause, stop), the play mode, the
 * battery (its icon, its percent or the music left: Settings); a line under it (row 16), row 17
 * blank. The title is drawn by head_title() when the screen is drawn in full; head_status()
 * redraws the right side's parts that changed (each pass), each pixel once ---- */
#define HEAD_LINE   RGB(60, 60, 70)
#define X_BAT       (LCD_W - 26)                /* the icon: 20 x 10 and its 2-pixel tip */
#define X_STATE     (LCD_W - 90)                /* icons of 10 and 13 pixels side by side */
#define X_MODE      (X_STATE + 10)
#define X_PCT0      (X_MODE + 13)               /* the percent or the time left, right-aligned */
#define X_CLOCK1    (X_STATE - 4)
#define X_CLOCK0    (X_CLOCK1 - 36)
#define X_EXTRA0    (X_CLOCK0 - 60)

static const uint8_t ic_play[] = { 0x0a, 0x08, 0x09, 0x00, 0x01, 0x80, 0xe0, 0xf8, 0xfe, 0xff,
                                   0xfe, 0xf8, 0xe0, 0x80 };
static const uint8_t ic_pause[] = { 0x0a, 0x08, 0x09, 0x00, 0x01, 0xe7, 0xe7, 0xe7, 0xe7, 0xe7,
                                    0xe7, 0xe7, 0xe7, 0xe7 };
static const uint8_t ic_stop[] = { 0x0a, 0x08, 0x08, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff };
static const uint8_t ic_none[] = { 0x0a, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t ic_repeat[] = { 0x0d, 0x0b, 0x08, 0x00, 0x01, 0x02, 0x00, 0x7f, 0xc0, 0x82,
                                     0x20, 0x80, 0x20, 0x80, 0x20, 0x88, 0x20, 0x7f, 0xc0, 0x08,
                                     0x00 };
static const uint8_t ic_repeat1[] = { 0x0d, 0x0b, 0x08, 0x00, 0x01, 0x02, 0x00, 0x7f, 0xc0, 0x82,
                                      0x20, 0x86, 0x20, 0x82, 0x20, 0x8a, 0x20, 0x7f, 0xc0, 0x08,
                                      0x00 };
static const uint8_t ic_shuffle[] = { 0x0d, 0x0b, 0x09, 0x00, 0x01, 0x00, 0x80, 0xc1, 0xe0, 0x22,
                                      0x80, 0x14, 0x00, 0x08, 0x00, 0x14, 0x00, 0x22, 0x80, 0xc1,
                                      0xe0, 0x00, 0x80 };
static const uint8_t ic_order[] = { 0x0d, 0x00, 0x00, 0x00, 0x00 };

static struct {
    int state, mode, pct, bat;                  /* as drawn; -1: to draw */
} hd = { -1, -1, -1, -1 };

void head_title(const char *title, uint16_t fg, const char *extra)
{
    int x1 = extra ? X_EXTRA0 : X_CLOCK0 - 6;
    lcd_fill(0, 0, 4, 16, BG);
    lcd_text_clip(4, 0, title, x1, fg, BG);
    if (extra)
        text_right(X_EXTRA0, X_CLOCK0 - 6, 0, extra, DIM, BG);
    lcd_fill(X_CLOCK0 - 6, 0, 6, 16, BG);
    lcd_fill(0, 16, LCD_W, 1, HEAD_LINE);
    lcd_fill(0, 17, LCD_W, 1, BG);
    hd.state = hd.mode = hd.pct = hd.bat = -1;  /* the right side drawn again in full */
    clock_min = -1;
}

void head_extra(const char *extra)              /* the "n/N" alone (a move in a list) */
{
    text_right(X_EXTRA0, X_CLOCK0 - 6, 0, extra, DIM, BG);
}

/* The battery's icon: an outline, a 1-pixel margin, the level inside (red when low, the bar's
 * colour while charging), its tip; each pixel once */
static void battery_icon(int pct, int low, int chg)
{
    int x = X_BAT, y = 3, w = 20, h = 10, in = (w - 4) * pct / 100;
    uint16_t ol = low ? WARN : FG, lv = low ? WARN : chg ? BAR_FG : FG;
    lcd_fill(X_PCT0, 0, LCD_W - X_PCT0, 3, BG);
    lcd_fill(X_PCT0, 3, x - X_PCT0, h, BG);
    lcd_fill(x, y, w, 1, ol);
    lcd_fill(x, y + h - 1, w, 1, ol);
    lcd_fill(x, y + 1, 1, h - 2, ol);
    lcd_fill(x + w - 1, y + 1, 1, h - 2, ol);
    lcd_fill(x + 1, y + 1, w - 2, 1, BG);                   /* the margin: top, bottom, */
    lcd_fill(x + 1, y + h - 2, w - 2, 1, BG);
    lcd_fill(x + 1, y + 2, 1, h - 4, BG);                   /* left, right */
    lcd_fill(x + w - 2, y + 2, 1, h - 4, BG);
    lcd_fill(x + 2, y + 2, in, h - 4, lv);
    lcd_fill(x + 2 + in, y + 2, w - 4 - in, h - 4, BG);
    lcd_fill(x + w, y, 2, 3, BG);
    lcd_fill(x + w, y + 3, 2, 4, ol);
    lcd_fill(x + w, y + 7, 2, 3, BG);
    lcd_fill(x + w + 2, 3, LCD_W - x - w - 2, h, BG);
    lcd_fill(X_PCT0, y + h, LCD_W - X_PCT0, 16 - y - h, BG);
}

/* The music left, from the percent: battery_pct() is already the share of the playing time left
 * (ETUDE §65), times a full run on battery (14.5 h, docs/BATTERY.md), less 10 % for the screen
 * and the keys, which the endurance runs hardly use: better more than shown than less. Whole
 * hours (rounded down), the minutes under an hour. */
#define BAT_FULL_MIN (145 * 6 * 9 / 10)         /* 14.5 h less 10 %: 783 minutes */
static void time_left_text(char *b, int pct)
{
    uint32_t m = (uint32_t)pct * BAT_FULL_MIN / 100;
    /* TRANSLATORS: the music left on the battery, in hours, then in minutes (header, short) */
    char *p = m >= 60 ? log_format(b, b + 7, T("%uh"), m / 60) : log_format(b, b + 7, T("%um"), m);
    *p = 0;
}

void head_status(int force)
{
    int state = !q_n ? 0 : q_playing && !audio_paused ? 1 : q_playing || parked ? 2 : 3;
    if (force || state != hd.state) {
        static const uint8_t *const ic[] = { ic_none, ic_play, ic_pause, ic_stop };
        lcd_fill(X_CLOCK1, 0, X_STATE - X_CLOCK1, 16, BG);
        lcd_icon(X_STATE, 0, ic[state], state == 1 ? TITLE_FG : DIM, BG);
        hd.state = state;
    }
    if (force || play_mode != hd.mode) {
        static const uint8_t *const ic[MODE_COUNT] = { ic_order, ic_repeat, ic_repeat1,
                                                       ic_shuffle };
        lcd_icon(X_MODE, 0, ic[play_mode], FG, BG);
        hd.mode = play_mode;
    }
    int low = !vbus() && battery_avg_mv() < 3425, chg = vbus() ? 1 + !!charging() : 0;
    int show = cfg.bat_show;
    if (show == BAT_TIME && chg)
        show = BAT_PCT;                         /* on the cable: no time left to tell */
    int pct = (int)battery_shown(), bat = show << 3 | low << 2 | chg;
    if (force || pct != hd.pct || bat != hd.bat) {
        if (show == BAT_ICON)
            battery_icon(pct, low, chg == 2);
        else {
            char b[8];
            if (show == BAT_TIME)
                time_left_text(b, pct);
            else {
                char *p = put_dec(b, (uint32_t)pct);
                *p++ = '%';
                *p = 0;
            }
            text_right(X_PCT0, LCD_W - 3, 0, b, low ? WARN : chg == 2 ? BAR_FG : FG, BG);
            lcd_fill(LCD_W - 3, 0, 3, 16, BG);
        }
        hd.pct = pct;
        hd.bat = bat;
    }
    draw_clock(X_CLOCK0, X_CLOCK1, force);
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

/* The column of a list's mark (22 pixels): the note of the track playing, or blank; each pixel
 * once (a fill of the column, then the note, flickered: ETUDE §144) */
void list_mark(int y, int on, uint16_t bg)
{
    if (!on) {
        lcd_fill(0, y, 22, 16, bg);
        return;
    }
    lcd_fill(0, y, 3, 16, bg);
    int w = lcd_glyph(3, y, 0x266a, TITLE_FG, bg);
    lcd_fill(3 + w, y, 22 - 3 - w, 16, bg);
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

