/* Latency measures: see perf.h. A phase is timed in segments, cut at every service of either
 * gap; each gap adds the segments of the phase running in its own window (a tree walk that keeps
 * the audio served still shows whole in the loops' gap). */
#define PERF_NO_CLOCK
#include "perf.h"

struct perf_gap perf_play, perf_loop;
static struct perf_gap *const gaps[] = { &perf_play, &perf_loop };
static const char *ph;                          /* the phase running, or 0 */
static uint32_t ph_t;                           /* the start of its current segment */

static void account(uint32_t now)
{
    uint32_t d = now - ph_t;
    ph_t = now;
    if (!ph)
        return;
    for (unsigned i = 0; i < sizeof gaps / sizeof gaps[0]; i++) {
        struct perf_gap *g = gaps[i];
        unsigned k = 0, low = 0;
        while (k < PERF_SLOTS && g->acc_ph[k] != ph) {
            if (g->acc_us[k] < g->acc_us[low])
                low = k;
            k++;
        }
        if (k == PERF_SLOTS) {                  /* a new phase: in the slot of the shortest */
            k = low;
            g->acc_ph[k] = ph;
            g->acc_us[k] = 0;
        }
        g->acc_us[k] += d;
        if (g->acc_us[k] > g->long_us) {
            g->long_us = g->acc_us[k];
            g->long_ph = ph;
        }
    }
}

const char *perf_begin_at(uint32_t now, const char *what)
{
    account(now);
    const char *outer = ph;
    ph = what;
    return outer;
}

void perf_end_at(uint32_t now, const char *outer)
{
    account(now);
    ph = outer;
}

void perf_tick_at(struct perf_gap *g, uint32_t now)
{
    account(now);
    if (g->last && now - g->last > g->max) {
        g->max = now - g->last;
        g->max_ph = g->long_us >= PERF_PHASE_MIN ? g->long_ph : 0;
        g->max_ph_us = g->max_ph ? g->long_us : 0;
    }
    g->long_ph = 0;
    g->long_us = 0;
    for (unsigned k = 0; k < PERF_SLOTS; k++)  /* the phase running starts again from 0 */
        g->acc_ph[k] = 0, g->acc_us[k] = 0;
    g->last = now;
}

void perf_reset(struct perf_gap *g)
{
    *g = (struct perf_gap){ 0 };
}

void perf_restart(struct perf_gap *g)
{
    uint32_t last = g->last;
    struct perf_gap k = *g;
    *g = (struct perf_gap){ 0 };
    g->last = last;                             /* the gap running goes on */
    g->long_ph = k.long_ph;
    g->long_us = k.long_us;
    for (unsigned i = 0; i < PERF_SLOTS; i++)
        g->acc_ph[i] = k.acc_ph[i], g->acc_us[i] = k.acc_us[i];
}
