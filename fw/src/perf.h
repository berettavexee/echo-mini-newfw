/* Latency measures for the rework (step 1 of the plan, ETUDE §97): the longest gap between two
 * services of the audio path (own_service) and of the main loops (sys_idle), each with the
 * longest named phase seen inside it (perf_begin / perf_end around the long operations). Read
 * only: nothing here changes what the firmware does. Pure logic, the clock passed in:
 * tools/perf_test.c */
#ifndef PERF_H
#define PERF_H

#include <stdint.h>

#define PERF_SLOTS 4             /* phases added up per window (the shortest gives way) */

struct perf_gap {
    uint32_t last;              /* the previous service, us (0: none yet) */
    uint32_t max;               /* the longest gap, us */
    const char *max_ph;         /* the longest phase inside it, or 0 */
    uint32_t max_ph_us;
    const char *long_ph;        /* the current window's longest phase so far */
    uint32_t long_us;
    const char *acc_ph[PERF_SLOTS];  /* this window's time per phase, added up: a phase cut by */
    uint32_t acc_us[PERF_SLOTS];     /* the other gap's services or by inner phases stays whole */
};

extern struct perf_gap perf_play, perf_loop;
#define PERF_PHASE_MIN 1000     /* us: a shorter phase is not named (the gap is elsewhere) */

/* A named phase starts (a string literal); returns the one it interrupts, for perf_end */
const char *perf_begin_at(uint32_t now, const char *what);
void perf_end_at(uint32_t now, const char *outer);
void perf_tick_at(struct perf_gap *g, uint32_t now);   /* a service: the gap since the last */
void perf_reset(struct perf_gap *g);
void perf_restart(struct perf_gap *g);  /* a new report: the maximum cleared, the gap running kept */

#ifndef PERF_NO_CLOCK
#include "board.h"
static inline const char *perf_begin(const char *what) { return perf_begin_at(ticks(), what); }
static inline void perf_end(const char *outer) { perf_end_at(ticks(), outer); }
static inline void perf_tick(struct perf_gap *g) { perf_tick_at(g, ticks()); }
#endif

#endif
