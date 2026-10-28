/* 320 x 170 panel on the VOP (MCU i8080 interface, 8-bit bus).
 *
 * Bus pins: D0-D7 = GPIO0_16..23, WR = GPIO0_24, RS = GPIO0_25, CS = GPIO0_15 (VOP function 1),
 * RD = GPIO0_12 (GPIO, only used to read the panel id). Reset = GPIO2_15.
 * The VOP is used in bypass mode: every write to VOP_DATA sends one byte, so the byte order of
 * the RGB565 pixels on the bus is the one written here (high byte first). */
#include "lcd.h"
#include "board.h"
#include "font.h"
#include "regs.h"

static uint32_t lcd_id;
static int lcd_variant;
int lcd_ready;

#define ROW_OFFSET 35   /* the 170 visible lines start at line 35 of the controller */

static inline void cmd(uint8_t c)
{
    REG(VOP_CMD) = c;
}

static inline void dat(uint8_t d)
{
    REG(VOP_DATA) = d;
}

/* Panel set-up tables: command, number of data bytes, data. 0xff = delay of 120 ms. */
static const uint8_t init_a[] = {
    0x11, 0xff,
    0x36, 1, 0x60,
    0x3a, 1, 0x05,
    0xa4, 2, 0x44, 0x44,
    0xb2, 5, 0x0c, 0x0c, 0x00, 0x33, 0x33,
    0xb7, 1, 0x35,
    0xbb, 1, 0x1a,
    0xc0, 1, 0x2c,
    0xc2, 1, 0x01,
    0xc3, 1, 0x0b,
    0xc4, 1, 0x20,
    0xc6, 1, 0x0f,
    0xd0, 2, 0xa4, 0xa1,
    0xd6, 1, 0xa1,
    0xe0, 14, 0xf0, 0x06, 0x0b, 0x07, 0x07, 0x24, 0x2e, 0x32, 0x46, 0x37, 0x13, 0x13, 0x2d, 0x33,
    0xe1, 14, 0xf0, 0x02, 0x06, 0x09, 0x08, 0x05, 0x29, 0x44, 0x42, 0x38, 0x14, 0x14, 0x2a, 0x30,
    0x2a, 4, 0x00, 0x00, 0x00, 0xef,
    0x2b, 4, 0x00, 0x23, 0x00, 0xcc,
    0x21, 0,
    0x29, 0,
    0x2c, 0,
};

static const uint8_t init_b[] = {
    0x11, 0xff,
    0xdf, 4, 0x5a, 0x69, 0x02, 0x01,
    0x36, 1, 0x60,
    0x3a, 1, 0x05,
    0xb2, 5, 0x0b, 0x0b, 0x00, 0x33, 0x33,
    0xb7, 1, 0x75,
    0xbb, 1, 0x28,
    0xc0, 1, 0x2c,
    0xc2, 1, 0x01,
    0xc3, 1, 0x1f,
    0xc5, 1, 0x2a,
    0xc6, 1, 0x13,
    0xd0, 1, 0xa7,
    0xd0, 2, 0xa4, 0xa1,
    0xd6, 1, 0xa1,
    0xe0, 14, 0xf0, 0x05, 0x0a, 0x06, 0x06, 0x03, 0x2b, 0x32, 0x43, 0x36, 0x11, 0x10, 0x2b, 0x32,
    0xe1, 14, 0xf0, 0x08, 0x0c, 0x0b, 0x09, 0x24, 0x2b, 0x22, 0x43, 0x38, 0x15, 0x16, 0x2f, 0x37,
    0x21, 0,
    0x29, 0,
    0x2c, 0,
};

static void send_table(const uint8_t *t, unsigned len)
{
    for (unsigned i = 0; i < len;) {
        cmd(t[i++]);
        unsigned n = t[i++];
        if (n == 0xff) {
            mdelay(120);
            continue;
        }
        while (n--)
            dat(t[i++]);
    }
}

static void panel_reset(uint32_t settle_ms)
{
    iomux(PIN_LCD_RESET, 0);
    gpio_dir(PIN_LCD_RESET, 1);
    gpio_set(PIN_LCD_RESET, 1);
    mdelay(50);
    gpio_set(PIN_LCD_RESET, 0);
    mdelay(150);
    gpio_set(PIN_LCD_RESET, 1);
    mdelay(settle_ms);
}

/* Bus pins as GPIO: RDDID (0x04), then a dummy byte and 3 id bytes, read with RD low. */
static uint32_t panel_read_id(void)
{
    uint32_t id = 0;

    iomux(0, 15, 0);                            /* CS low for the whole exchange */
    gpio_dir(0, 15, 1);
    gpio_set(0, 15, 0);
    for (int p = 16; p < 24; p++) {
        iomux(0, p, 0);
        gpio_dir(0, p, 1);
    }
    iomux(0, 12, 0);                            /* RD high */
    gpio_dir(0, 12, 1);
    gpio_set(0, 12, 1);
    iomux(0, 24, 0);                            /* WR low, RS low: command */
    gpio_dir(0, 24, 1);
    gpio_set(0, 24, 0);
    iomux(0, 25, 0);
    gpio_dir(0, 25, 1);
    gpio_set(0, 25, 0);
    REG(GPIO0_BASE + GPIO_DR) = (REG(GPIO0_BASE + GPIO_DR) & ~0x00ff0000u) | (0x04u << 16);
    mdelay(1);
    gpio_set(0, 24, 1);                         /* WR rising edge latches the command */

    for (int p = 16; p < 24; p++)
        gpio_dir(0, p, 0);
    gpio_set(0, 25, 1);                         /* RS high: data */
    mdelay(1);
    for (int i = 0; i < 4; i++) {
        gpio_set(0, 12, 0);
        mdelay(1);
        id = id << 8 | ((REG(GPIO0_BASE + GPIO_EXT) >> 16) & 0xff);
        gpio_set(0, 12, 1);
    }
    gpio_dir(0, 12, 0);
    return id;
}

static void bus_to_vop(void)
{
    for (int p = 16; p < 26; p++)
        iomux(0, p, 1);
    iomux(0, 15, 1);
    iomux(0, 12, 0);
    gpio_dir(0, 12, 0);
}

/* Timing of the MCU interface from the bus clock, as the stock firmware computes it:
 * n = hclk / 8 MHz, CS-to-strobe = n / 8, strobe width = n / 6, strobe-to-CS = n / 8. */
static void lcd_set_timing(uint32_t hclk)
{
    uint32_t n = hclk / 8000000, csrw = n >> 3, rwpw = n / 6, rwcs = n >> 3;

    if (csrw > 0x3f)
        csrw = 0xf;
    if (rwpw == 0)
        rwpw = 1;
    if (rwpw > 0x3f)
        rwpw = 0x3f;
    if (rwcs == 0)
        rwcs = 1;
    if (rwcs > 0x1f)
        rwcs = 0x1f;
    REG(VOP_TIMING) = csrw << 12 | rwpw << 5 | rwcs;
}

static void window(int x0, int y0, int x1, int y1)
{
    y0 += ROW_OFFSET;
    y1 += ROW_OFFSET;
    cmd(0x2a);
    dat(x0 >> 8);
    dat(x0);
    dat(x1 >> 8);
    dat(x1);
    cmd(0x2b);
    dat(y0 >> 8);
    dat(y0);
    dat(y1 >> 8);
    dat(y1);
    cmd(0x2c);
}

static inline void pixel(uint16_t c)
{
    dat(c >> 8);
    dat(c);
}

void lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0)
        return;
    window(x, y, x + w - 1, y + h - 1);
    for (int n = w * h; n; n--)
        pixel(color);
}

static int glyph_width(const uint8_t *g)
{
    return g[0];                                /* the advance */
}

/* A glyph record (font.h) in a line of 16 px times 'sc' (1 or 2: each pixel a square): baseline
 * at row font_ascent, the advance filled */
/* Only the columns in [cl, cr) of the screen are drawn (a scrolling line) */
static void draw_glyph_cols(int x, int y, const uint8_t *g, uint16_t fg, uint16_t bg, int sc,
                            int cl, int cr)
{
    int adv = g[0], w = g[1], h = g[2], xo = (int8_t)g[3], yo = (int8_t)g[4];
    int bpr = (w + 7) / 8, top = font_ascent - yo - h;  /* row of the glyph's first line */
    int c0 = cl > x ? cl - x : 0, c1 = x + adv * sc > cr ? cr - x : adv * sc;
    const uint8_t *bits = g + 5;
    if (c1 <= c0)
        return;
    window(x + c0, y, x + c1 - 1, y + FONT_LINE * sc - 1);
    for (int r = 0; r < FONT_LINE * sc; r++) {
        int gy = r / sc - top;
        const uint8_t *row = gy >= 0 && gy < h ? bits + gy * bpr : 0;
        for (int c = c0; c < c1; c++) {
            int gx = c / sc - xo;
            pixel(row && gx >= 0 && gx < w && (row[gx >> 3] & (0x80 >> (gx & 7))) ? fg : bg);
        }
    }
}

static int draw_glyph(int x, int y, const uint8_t *g, uint16_t fg, uint16_t bg, int sc)
{
    int adv = g[0] * sc;
    if (!adv || x + adv > LCD_W)
        return 0;
    draw_glyph_cols(x, y, g, fg, bg, sc, x, x + adv);
    return adv;
}

/* s scrolled left by 'off' pixels in [x, xmax): s, a gap of 'gap' pixels, s again */
void lcd_text_scroll(int x, int y, const char *s, int xmax, int off, int gap, uint16_t fg,
                     uint16_t bg)
{
    int pos = x - off;
    for (int copy = 0; copy < 2 && pos < xmax; copy++) {
        for (const char *t = s; *t && pos < xmax;) {
            const uint8_t *g = font_glyph(utf8_next(&t));
            if (pos + g[0] > x)
                draw_glyph_cols(pos, y, g, fg, bg, 1, x, xmax);
            pos += g[0];
        }
        if (!copy) {
            int a = pos > x ? pos : x, b = pos + gap < xmax ? pos + gap : xmax;
            if (b > a)
                lcd_fill(a, y, b - a, FONT_LINE, bg);
            pos += gap;
        }
    }
    if (pos < xmax)
        lcd_fill(pos > x ? pos : x, y, xmax - (pos > x ? pos : x), FONT_LINE, bg);
}

int lcd_glyph(int x, int y, uint32_t cp, uint16_t fg, uint16_t bg)
{
    return draw_glyph(x, y, font_glyph(cp), fg, bg, 1);
}

uint32_t utf8_next(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s++;
    int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (c >= 0x80 && !n)
        c = 0xfffd;                             /* a stray continuation byte */
    else if (n)
        c &= 0x3f >> n;
    while (n-- > 0) {
        if ((*s & 0xc0) != 0x80) {
            c = 0xfffd;
            break;
        }
        c = c << 6 | (*s++ & 0x3f);
    }
    *ps = (const char *)s;
    return c;
}

int lcd_text_width_x(const char *s, int sc)
{
    int w = 0;
    while (*s)
        w += glyph_width(font_glyph(utf8_next(&s))) * sc;
    return w;
}

int lcd_text_width(const char *s)
{
    return lcd_text_width_x(s, 1);
}

/* UTF-8 text from x up to xmax (excluded), cut with an ellipsis when it does not fit; the rest
 * of the band up to xmax is filled with bg. Returns the x reached. */
int lcd_text_clip_x(int x, int y, const char *s, int xmax, uint16_t fg, uint16_t bg, int sc)
{
    int ell = glyph_width(font_glyph(0x2026)) * sc;
    int fits = x + lcd_text_width_x(s, sc) <= xmax;
    while (*s) {
        const char *t = s;
        uint32_t c = utf8_next(&t);
        int w = glyph_width(font_glyph(c)) * sc;
        if (!fits && x + w + ell > xmax) {
            x += draw_glyph(x, y, font_glyph(0x2026), fg, bg, sc);
            break;
        }
        x += draw_glyph(x, y, font_glyph(c), fg, bg, sc);
        s = t;
    }
    if (x < xmax)
        lcd_fill(x, y, xmax - x, FONT_LINE * sc, bg);
    return x;
}

int lcd_text_clip(int x, int y, const char *s, int xmax, uint16_t fg, uint16_t bg)
{
    return lcd_text_clip_x(x, y, s, xmax, fg, bg, 1);
}

int lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    while (*s)
        x += lcd_glyph(x, y, utf8_next(&s), fg, bg);
    return x;
}

void lcd_backlight(int on)
{
    iomux(PIN_BACKLIGHT, 0);                    /* GPIO: full brightness, no PWM yet */
    gpio_dir(PIN_BACKLIGHT, 1);
    gpio_set(PIN_BACKLIGHT, on);
}

/* Panel sleep (ST7789 DISPOFF + SLPIN, then SLPOUT + 120 ms + DISPON): the controller keeps its
 * memory, and the interface still writes it, so drawing may go on while it sleeps */
static int lcd_asleep;

void lcd_sleep(int on)
{
    if (on == lcd_asleep)
        return;
    if (on) {
        cmd(0x28);
        cmd(0x10);
        mdelay(5);
    } else {
        cmd(0x11);
        mdelay(120);
        cmd(0x29);
    }
    lcd_asleep = on;
}

/* The fault screen: no SysTick there, a counted wait instead of mdelay() */
void lcd_wake_fault(void)
{
    if (!lcd_asleep)
        return;
    cmd(0x11);
    for (volatile uint32_t i = 0; i < 6000000; i++)
        ;
    cmd(0x29);
    lcd_asleep = 0;
}

void lcd_init(void)
{
    clk_gate(GATE_HCLK_VOP, 1);
    soft_reset(SRST_VOP, 0);
    lcd_set_timing(cpu_hz);

    panel_reset(30);
    iomux(PIN_LCD_SEL, 0);                      /* variant pin, read before power-up */
    gpio_dir(PIN_LCD_SEL, 0);
    udelay(10);
    int sel = gpio_get(PIN_LCD_SEL);
    gpio_dir(PIN_LCD_SEL, 1);
    gpio_set(PIN_LCD_SEL, 0);
    lcd_id = panel_read_id();
    if (!sel)
        lcd_id = 0x66666666;
    bus_to_vop();

    panel_reset(120);
    REG(VOP_CON) = VOP_CON_BYPASS;
    lcd_variant = (lcd_id & 0xff0000) == 0xe80000;
    if (lcd_variant)
        send_table(init_b, sizeof init_b);
    else
        send_table(init_a, sizeof init_a);
    lcd_fill(0, 0, LCD_W, LCD_H, 0);
    lcd_ready = 1;
}
