/* Keys and plugs (ETUDE §3.2, §34, §42): the resistor ladder on ADC channel 1 debounced into one
 * key at a time, then turned into events for the screens: a short press (on release), a long
 * press (held 600 ms), repeats of VOL+/VOL- held, and the VOL+/VOL- chord; also the headphone
 * plugs (3.5 mm, 4.4 mm) and the USB cable going in or out. Power is read apart (sys_idle). */
#ifndef KEYS_H
#define KEYS_H

#include <stdint.h>

enum { EV_NONE, EV_PRESS, EV_LONG, EV_REPEAT, EV_CHORD, EV_PLUG, EV_UNPLUG };

/* key_ev.key of EV_PLUG and EV_UNPLUG */
enum { SRC_JACK35 = 16, SRC_JACK44, SRC_USB };

struct key_ev {
    int key;                    /* KEY_* (board.h); for EV_CHORD the key pressed first; SRC_* for
                                 * EV_PLUG and EV_UNPLUG */
    int type;                   /* EV_* */
};

int key_poll(struct key_ev *e); /* the next event (e->type, also returned), EV_NONE: none */
int key_held(void);             /* a key is down, as key_poll() sees it */
uint32_t key_held_us(void);     /* since the key held (or the last change) */
int key_is_vol(int k);          /* VOL+ or VOL- */
const char *key_name(int k);

/* Simple screens (system, installer): the key newly down, once, else KEY_NONE */
int key_edge(void);
void key_edge_sync(void);       /* the key down now is not new */

#endif
