/* Host-side test of src/remote.c: the headset remote's double press of VOL+ and VOL-.
 *
 *   cc -O1 -Wall -Isrc -o build/remote_test tools/remote_test.c && build/remote_test */
#include <stdio.h>
#include "../src/remote.c"

static int fails;

static void expect(const char *what, int got, int want, unsigned bits, unsigned want_bits)
{
    if (got != want || bits != want_bits) {
        printf("FAIL %s: %d bits %u, expected %d bits %u\n", what, got, bits, want, want_bits);
        fails++;
    }
}

int main(void)
{
    struct rc_state s = { 0 };
    unsigned b;
    int r;

    r = rc_vol(&s, 1, 0, 1, 1000, &b);
    expect("first VOL+", r, RC_STEP, b, 0);
    r = rc_vol(&s, 1, 0, 1, 1300, &b);
    expect("VOL+ twice: next", r, RC_UNDO, b, RC_NEXT);
    r = rc_vol(&s, 1, 0, 1, 1500, &b);
    expect("a third VOL+: a step", r, RC_STEP, b, 0);

    r = rc_vol(&s, -1, 0, 1, 5000, &b);
    expect("first VOL-", r, RC_STEP, b, 0);
    r = rc_vol(&s, -1, 0, 1, 5000 + RC_DOUBLE_MS, &b);
    expect("VOL- twice at the limit: previous", r, RC_UNDO, b, RC_PREV);

    rc_vol(&s, 1, 0, 1, 8000, &b);
    r = rc_vol(&s, 1, 0, 1, 8000 + RC_DOUBLE_MS + 1, &b);
    expect("VOL+ twice, too slow: a step", r, RC_STEP, b, 0);

    rc_vol(&s, 1, 0, 1, 10000, &b);
    r = rc_vol(&s, -1, 0, 1, 10100, &b);
    expect("VOL+ then VOL-: a step", r, RC_STEP, b, 0);

    rc_vol(&s, 1, 0, 1, 12000, &b);
    rc_vol(&s, 1, 1, 1, 12100, &b);
    r = rc_vol(&s, 1, 0, 1, 12200, &b);
    expect("VOL+, held, VOL+: a step", r, RC_STEP, b, 0);

    rc_vol(&s, 1, 0, 0, 14000, &b);
    r = rc_vol(&s, 1, 0, 0, 14100, &b);
    expect("remote off: a step", r, RC_STEP, b, 0);

    s.dir = 0;
    rc_vol(&s, 1, 0, 1, 0xfffffff0u, &b);
    r = rc_vol(&s, 1, 0, 1, 0x100u, &b);
    expect("VOL+ twice across the millis() wrap: next", r, RC_UNDO, b, RC_NEXT);

    printf("%s\n", fails ? "remote_test: FAIL" : "remote_test: ok");
    return fails != 0;
}
