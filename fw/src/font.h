/* Font: Fusion Pixel 12px proportional (SIL OFL 1.1, font/fusion-pixel-12px), 16 px lines
 * (ascent 13, descent 3). The glyphs live in our firmware image on the eMMC (tools/mkfusion.py,
 * header field 0x80) and are read on demand into a cache; printable ASCII is built in, for the
 * time before font_init() and for the fault screen.
 * Glyph record: u8 advance, u8 w, u8 h, s8 x offset, s8 y offset (bottom row above the
 * baseline), then h rows of (w+7)/8 bytes, leftmost pixel in bit 7. */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>

#define FONT_LINE 16

extern const uint8_t font_ascii[];
extern const uint16_t font_ascii_off[95];
extern const uint8_t font_ascent;

int font_init(void);                    /* reads the tables from FW1 (else FW2): 0, or < 0 */
/* The record of code point c ('?' when the font does not have it); valid until the next call */
const uint8_t *font_glyph(uint32_t c);
uint32_t font_misses(void);             /* groups of 16 glyphs read from the eMMC so far */

#endif
