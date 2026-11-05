/* The USB sound card's keys for the host, as a wired or Bluetooth headset's remote (a HID
 * consumer control, uac.c): PLAY play/pause, VOL+ twice next track, VOL- twice previous track.
 * A volume key's first press changes the volume at once; a second press of the same key within
 * RC_DOUBLE_MS undoes that step and sends the track key instead. Pure logic: tools/remote_test.c */
#ifndef REMOTE_H
#define REMOTE_H

#include <stdint.h>

#define RC_PLAY      1u         /* report bits, in the order of the report descriptor */
#define RC_NEXT      2u
#define RC_PREV      4u
#define RC_DOUBLE_MS 400u       /* from one release to the next */

enum { RC_STEP, RC_UNDO };      /* rc_vol(): one volume step, or the last step undone */

struct rc_state {
    int dir;                    /* the last single press (+1 up, -1 down), 0: none */
    uint32_t at;                /* its time, ms */
};

/* A volume key: dir +1 louder, -1 quieter; held: a repeat; on: the remote on. RC_STEP: a step in
 * dir; RC_UNDO: the previous press's step undone and *bits sent. *bits is 0 but for RC_UNDO */
int rc_vol(struct rc_state *s, int dir, int held, int on, uint32_t ms, unsigned *bits);

#endif
