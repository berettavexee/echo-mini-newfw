/* Host-side test of the glyph renderer of src/lcd.c (ETUDE §134): every glyph of the font blob
 * of the build, at sizes 1 and 2, cut at columns and drawn into strips of every height, compared
 * pixel for pixel with the renderer of 0.103 (each pixel tested: kept below as the reference).
 *   cc -O1 -Wall -Isrc -Ibuild -o build/lcd_test tools/lcd_test.c && build/lcd_test build/font.bin */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "regs.h"
#undef REG
static volatile uint32_t host_regs[1024];
#define REG(a) (host_regs[((a) >> 2) & 1023])
#include "../src/font.c"
#include "font_ascii.c"
uint32_t cpu_hz = 200000000;
uint32_t ticks(void) { return 0; }
uint32_t millis(void) { return 0; }
void mdelay(uint32_t ms) { (void)ms; }
void udelay(uint32_t us) { (void)us; }
void iomux(int b, int p, int f) { (void)b, (void)p, (void)f; }
void gpio_dir(int b, int p, int o) { (void)b, (void)p, (void)o; }
void gpio_set(int b, int p, int v) { (void)b, (void)p, (void)v; }
int gpio_get(int b, int p) { (void)b, (void)p; return 0; }
void clk_gate(int n, int on) { (void)n, (void)on; }
void soft_reset(int n, int on) { (void)n, (void)on; }
#include "../src/lcd.c"

struct emmc_geom emmc = { .p = 0x10000, .r = 0x8000 };
static uint8_t *img;
static uint32_t img_n;

int emmc_read(uint32_t sector, uint32_t count, void *buf)
{
    if (sector < EMMC_FW1 || (sector - EMMC_FW1 + count) * 512 > img_n) {
        memset(buf, 0, 512 * count);
        return 0;
    }
    memcpy(buf, img + (sector - EMMC_FW1) * 512, 512 * count);
    return 0;
}

void log_printf(const char *fmt, ...)
{
    (void)fmt;
}

/* the renderer of 0.103: pixel (r, c) of a glyph cell at (x, y), columns [cl, cr) */
static int ref_pixel(const uint8_t *g, int sc, int x, int y, int r, int c, uint16_t fg,
                     uint16_t bg)
{
    int w = g[1], h = g[2], xo = (int8_t)g[3], yo = (int8_t)g[4];
    int bpr = (w + 7) / 8, top = font_ascent - yo - h;
    const uint8_t *bits = g + 5;
    int gy = (r - y) / sc - top, gx = (c - x) / sc - xo;
    const uint8_t *row = gy >= 0 && gy < h ? bits + gy * bpr : 0;
    return row && gx >= 0 && gx < w && (row[gx >> 3] & (0x80 >> (gx & 7))) ? fg : bg;
}

static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f) {
        fprintf(stderr, "usage: lcd_test build/font.bin\n");
        return 2;
    }
    static uint8_t blob[4 << 20];
    uint32_t blen = (uint32_t)fread(blob, 1, sizeof blob, f);
    fclose(f);
    img_n = (0x1000 + blen + 511) & ~511u;
    img = calloc(1, img_n);
    img[0x1f8] = 'R';
    img[0x1ff] = 'W';
    memcpy(img + 0x80, &(uint32_t){ 0x1000 }, 4);
    memcpy(img + 0x84, &blen, 4);
    memcpy(img + 0x1000, blob, blen);
    if (font_init()) {
        printf("FAIL font_init\n");
        return 1;
    }
    uint32_t nr = g32(blob + 12), of = g32(blob + 0x14), oi = g32(blob + 0x18);
    long checked = 0, bad = 0;
    const uint16_t fg = 0xffe0, bg = 0x0010;
    for (uint32_t run = 0; run < nr; run++) {
        uint32_t c0 = g32(blob + of + 4 * run);
        uint32_t n = (blob[oi + 2 * run + 2] | blob[oi + 2 * run + 3] << 8) -
                     (blob[oi + 2 * run] | blob[oi + 2 * run + 1] << 8);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t cp = c0 + k;
            const uint8_t *g = font_glyph(cp);
            int adv = g[0];
            for (int sc = 1; sc <= 2; sc++) {
                int x = 37 + (int)(cp % 7), y = 5 + (int)(cp % 3);
                /* the cell whole, then cut on the left and on the right (a scrolling line) */
                int cuts[3][2] = { { x, x + adv * sc }, { x + 1 + (int)(cp % 5), LCD_W },
                                   { 0, x + adv * sc - 1 - (int)(cp % 3) } };
                for (int cu = 0; cu < 3; cu++) {
                    int cl = cuts[cu][0], cr = cuts[cu][1];
                    /* strips of 8 rows, then of 3 (rows cut anywhere) */
                    for (int sh = 8; sh >= 3; sh -= 5)
                        for (int sy = y - 2; sy < y + FONT_LINE * sc; sy += sh) {
                            strip_begin(20, 120, sy, sy + sh, 0x1234);
                            draw_glyph_cols(x, y, g, fg, bg, sc, cl, cr);
                            for (int r = sy; r < sy + sh; r++)
                                for (int c = 20; c < 120; c++) {
                                    uint16_t got = st.p[(r - sy) * 100 + (c - 20)];
                                    int in = r >= y && r < y + FONT_LINE * sc && c >= x &&
                                             c < x + adv * sc && c >= cl && c < cr;
                                    uint16_t want = in ? (uint16_t)ref_pixel(g, sc, x, y, r, c, fg, bg)
                                                       : 0x1234;
                                    checked++;
                                    if (got != want && bad++ < 10)
                                        printf("FAIL U+%04X size %d cut %d strip %d+%d: (%d, %d) %04x, "
                                               "want %04x\n", cp, sc, cu, sy, sh, c, r, got, want);
                                }
                        }
                }
            }
        }
    }
    printf("%s %ld pixels compared, %ld different\n", bad ? "FAIL" : "ok  ", checked, bad);
    return bad != 0;
}
