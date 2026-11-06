/* Host-side test of src/perf.c: gaps and the phases named inside them.
 *
 *   cc -O1 -Wall -Isrc -o build/perf_test tools/perf_test.c && build/perf_test */
#include <stdio.h>
#include <string.h>
#include "../src/perf.c"

static int fails;

static void expect(const char *what, const struct perf_gap *g, uint32_t max, const char *ph,
                   uint32_t ph_us)
{
    if (g->max != max || (ph ? !g->max_ph || strcmp(g->max_ph, ph) : g->max_ph != 0) ||
        g->max_ph_us != ph_us) {
        printf("FAIL %s: max %u phase %s %u, expected %u %s %u\n", what, g->max,
               g->max_ph ? g->max_ph : "-", g->max_ph_us, max, ph ? ph : "-", ph_us);
        fails++;
    }
}

int main(void)
{
    perf_tick_at(&perf_play, 1000);
    perf_tick_at(&perf_play, 3000);
    expect("a plain gap", &perf_play, 2000, 0, 0);

    const char *o = perf_begin_at(4000, "tags");
    perf_end_at(104000, o);
    perf_tick_at(&perf_play, 110000);
    expect("a phase inside the gap", &perf_play, 107000, "tags", 100000);

    perf_tick_at(&perf_play, 111000);
    expect("a short gap after it: the maximum kept", &perf_play, 107000, "tags", 100000);

    o = perf_begin_at(200000, "folder");             /* nested: the card mounts inside */
    const char *o2 = perf_begin_at(210000, "card mount");
    perf_end_at(260000, o2);
    perf_end_at(400000, o);
    perf_tick_at(&perf_play, 400000);
    expect("nested phases: the outer one added up", &perf_play, 289000, "folder", 150000);

    perf_reset(&perf_play);
    perf_tick_at(&perf_play, 500000);
    o = perf_begin_at(500000, "draw");                /* a phase cut by a service */
    perf_tick_at(&perf_play, 510000);
    perf_end_at(530000, o);
    perf_tick_at(&perf_play, 540000);
    expect("a phase across a service", &perf_play, 30000, "draw", 20000);

    perf_reset(&perf_play);                           /* a walk serving the audio: the loops */
    perf_reset(&perf_loop);                           /* see it whole, the audio in pieces */
    perf_tick_at(&perf_play, 600000);
    perf_tick_at(&perf_loop, 600000);
    o = perf_begin_at(600000, "tree walk");
    for (uint32_t t = 610000; t <= 1600000; t += 10000)
        perf_tick_at(&perf_play, t);
    perf_end_at(1600000, o);
    perf_tick_at(&perf_loop, 1600000);
    expect("a long phase across audio services: loop", &perf_loop, 1000000, "tree walk", 1000000);
    expect("a long phase across audio services: audio", &perf_play, 10000, "tree walk", 10000);

    perf_reset(&perf_loop);                           /* the walk with the next tags inside it */
    perf_tick_at(&perf_loop, 2000000);
    o = perf_begin_at(2000000, "tree walk");
    for (uint32_t t = 2000000; t < 3000000; t += 100000) {
        const char *o2 = perf_begin_at(t + 60000, "tags");
        perf_end_at(t + 90000, o2);
    }
    perf_end_at(3000000, o);
    perf_tick_at(&perf_loop, 3000000);
    expect("a walk cut by inner phases: added up", &perf_loop, 1000000, "tree walk", 700000);

    perf_reset(&perf_loop);
    perf_tick_at(&perf_loop, 0xfffff000u);
    perf_tick_at(&perf_loop, 0x00001000u);
    expect("across the wrap of ticks()", &perf_loop, 0x2000, 0, 0);

    printf("%s\n", fails ? "perf_test: FAIL" : "perf_test: ok");
    return fails != 0;
}
