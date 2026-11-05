/* The headset remote's volume keys: see remote.h */
#include "remote.h"

int rc_vol(struct rc_state *s, int dir, int held, int on, uint32_t ms, unsigned *bits)
{
    *bits = 0;
    if (held || !on) {                  /* held: the volume only, and no double press after it */
        s->dir = 0;
        return RC_STEP;
    }
    if (s->dir == dir && ms - s->at <= RC_DOUBLE_MS) {
        s->dir = 0;                     /* a third press starts again */
        *bits = dir > 0 ? RC_NEXT : RC_PREV;
        return RC_UNDO;
    }
    s->dir = dir;
    s->at = ms;
    return RC_STEP;
}
