/* User interface: browser, now playing, main menu (ETUDE §25) */
#ifndef UI_H
#define UI_H

void ui_main(void);             /* never returns */
void ui_background(void);       /* the player, between two key polls or during long reads */
extern int play_level;
void ui_power_off(void);        /* before switching off: the resume state */          /* PCM_VOL attenuation (1/2 dB steps) */

#endif
