/* 320 x 170 panel (ST7789-type controller) on the VOP, i8080 8-bit bus. */
#ifndef LCD_DRV_H
#define LCD_DRV_H

#include <stdint.h>

#define LCD_W 320
#define LCD_H 170

#define RGB(r, g, b) ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))

extern uint32_t lcd_id;
extern int lcd_variant;         /* 0 = A, 1 = B (id & 0xff0000 == 0xe80000) */
extern int lcd_ready;

void lcd_init(void);
void lcd_set_timing(uint32_t hclk);
void lcd_fill(int x, int y, int w, int h, uint16_t color);
int lcd_char(int x, int y, char c, uint16_t fg, uint16_t bg);  /* returns the advance */
int lcd_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);
/* UTF-8 */
uint32_t utf8_next(const char **s);
int lcd_glyph(int x, int y, uint32_t cp, uint16_t fg, uint16_t bg);
int lcd_text_width(const char *s);
int lcd_text_clip(int x, int y, const char *s, int xmax, uint16_t fg, uint16_t bg);
/* the same, each pixel of the font a square of sc x sc (lines of 16 x sc pixels) */
int lcd_text_width_x(const char *s, int sc);
void lcd_text_scroll(int x, int y, const char *s, int xmax, int off, int gap, uint16_t fg,
                     uint16_t bg);  /* s scrolled left by off pixels, then gap, then s again */
int lcd_text_clip_x(int x, int y, const char *s, int xmax, uint16_t fg, uint16_t bg, int sc);
void lcd_backlight(int on);
void lcd_sleep(int on);         /* panel asleep (backlight apart); waking takes 120 ms */
void lcd_wake_fault(void);

#endif
