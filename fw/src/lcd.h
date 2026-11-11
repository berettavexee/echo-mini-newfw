/* 320 x 170 panel (ST7789-type controller) on the VOP, i8080 8-bit bus. */
#ifndef LCD_DRV_H
#define LCD_DRV_H

#include <stdint.h>

#define LCD_W 320
#define LCD_H 170

#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

extern int lcd_ready;

void lcd_init(void);
void lcd_perf_probe(void);       /* test build ECHO_LCDPERF: the screen's costs, logged */
void lcd_calibrate(void);        /* test build ECHO_LCDCAL: the VOP mode for the DMA, picked */
/* The output by DMA (ETUDE §134): the transfer in flight finished, the VOP back to the CPU; before
 * anything else uses DMAC1 (a reset), the clocks or the panel */
void lcd_sync(void);
extern int lcd_dma_mode;         /* 0: the CPU writes each byte; 1-4: DMA, the VOP's mode */
extern int lcd_lost;             /* the DMA failed: the screen to redraw in full (cleared there) */
struct lcd_dma_stats {
    uint32_t xfers, bytes, wait_us, wait_max, fails;
};
extern struct lcd_dma_stats lcd_dma;
void lcd_fill(int x, int y, int w, int h, uint16_t color);
int lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);
/* UTF-8 */
uint32_t utf8_next(const char **s);
int lcd_glyph(int x, int y, uint32_t cp, uint16_t fg, uint16_t bg);
int lcd_text_width(const char *s);
int lcd_text_clip(int x, int y, const char *s, int xmax, uint16_t fg, uint16_t bg);
/* the same, each pixel of the font a square of sc x sc (lines of 16 x sc pixels) */
int lcd_text_width_x(const char *s, int sc);
/* s scrolled left by off pixels, then gap, then s again */
void lcd_text_scroll(int x, int y, const char *s, int xmax, int off, int gap, int sc,
                     uint16_t fg, uint16_t bg);
int lcd_text_clip_x(int x, int y, const char *s, int xmax, uint16_t fg, uint16_t bg, int sc);
void lcd_backlight(int on);
void lcd_brightness(int pct);   /* 1 to 100 %, applied at once when the backlight is on */
void lcd_sleep(int on);         /* panel asleep (backlight apart): asked, done by lcd_service() */
void lcd_wake_lit(void);        /* awake, then the backlight on, once the panel's 120 ms passed */
void lcd_service(void);         /* the panel's sleep and wake commands, each after its delay */
void lcd_settle(void);          /* waits for it (up to 120 ms): before a power off, a fixed wait */
void lcd_wake_fault(void);

#endif
