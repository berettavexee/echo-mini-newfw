/* User interface: browser, now playing, main menu (ETUDE §25) */
#ifndef UI_H
#define UI_H

void ui_main(void);             /* never returns */
void ui_background(void);       /* the player, between two key polls or during long reads */
extern int play_level;
void ui_power_off(void);        /* before switching off: the resume state */          /* PCM_VOL attenuation (1/2 dB steps) */

/* Lists of rows (uidraw.c): the selection moves by d and wraps around, the first row shown
 * (top) follows it; past a screen, a scroll bar at the right edge of the rows (only then: the
 * rows' text stops at list_text_end) */
void list_move(int *sel, int *top, int n, int rows, int d);
int list_text_end(int n, int rows);
void list_scrollbar(int y, int rows, int top, int n);

#endif
