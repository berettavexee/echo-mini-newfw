/* Keys: debouncing of the ladder, events for the screens (keys.h) */
#include <stdint.h>
#include "keys.h"
#include "board.h"
#include "log.h"

/* ---- debouncing, in the SysTick interrupt (ETUDE §140) ----
 * The ladder is RC filtered: pressing VOL-, VOL+ or Play sweeps channel 1 down through the
 * Menu range for a few ms. A key counts once its class has been the same for 30 ms. A key held
 * (0.53, ETUDE §34): a release counts after 60 ms (a loose contact of Play reads none for a few
 * ms; VOL+/VOL-: 30 ms, quick clicks leave 50-60 ms between them, 0.54), VOL+ <-> VOL- (the chord)
 * after 100 ms, and another key not at all until a release (a loose contact of Play sweeps up
 * through Menu and Back: these read as Back presses in 0.52).
 * keys_sample() runs every 2 ms (board.c, adc_tick), whatever the loop does: each change of the
 * debounced key goes into a queue with its time and reading, and the loop turns the changes into
 * events at their own times. A press while the loop is held (a card mounted, a folder opened) is
 * no longer lost. Nothing is logged from the interrupt: what the loop should log waits here. */
#define KEY_SETTLE_US   30000
#define KEY_RELEASE_US  60000
#define KEY_CHORD_US   100000

struct key_change {
    uint32_t t;                                 /* ticks() when it settled */
    uint16_t adc;                               /* channel 1 then */
    uint8_t key;                                /* KEY_* or KEY_NONE (released) */
};
#define KQ 16                                   /* a power of 2 (the indexes wrap at 256) */
static struct key_change kq[KQ];
static volatile uint8_t kq_w, kq_r;             /* written by the interrupt, by the loop */
static volatile uint8_t kq_lost;                /* changes dropped, the queue full */
static volatile uint8_t ign_key, ign_raw;       /* a reading ignored while a key was held, */
static volatile uint16_t ign_ms;                /* for the loop to log (0: none) */

static const char *const key_names[] = { "none", "vol-", "vol+", "play", "menu", "back" };

const char *key_name(int k)
{
    return key_names[k];
}

void keys_sample(unsigned v, uint32_t now)
{
    static int raw = KEY_NONE, stable = KEY_NONE;
    static uint32_t since;
    int k = key_ladder(v);

    if (k != raw) {
        uint32_t held = now - since;
        if (raw != stable && stable != KEY_NONE && held >= KEY_SETTLE_US / 2 && !ign_ms) {
            ign_key = (uint8_t)stable;
            ign_raw = (uint8_t)raw;
            ign_ms = (uint16_t)(held / 1000 > 0xffff ? 0xffff : held / 1000);
        }
        raw = k;
        since = now;
        return;
    }
    if (k == stable)
        return;
    uint32_t need = stable == KEY_NONE ? KEY_SETTLE_US :
                    k == KEY_NONE ? (stable == KEY_VOLUP || stable == KEY_VOLDOWN ?
                                     KEY_SETTLE_US : KEY_RELEASE_US) :
                    (stable == KEY_VOLUP || stable == KEY_VOLDOWN) &&
                    (k == KEY_VOLUP || k == KEY_VOLDOWN) ? KEY_CHORD_US : 0xffffffffu;
    if (now - since < need)
        return;
    stable = k;
    uint8_t w = kq_w;
    if ((uint8_t)(w - kq_r) >= KQ) {
        kq_lost++;
        return;
    }
    kq[w % KQ] = (struct key_change){ .t = now, .adc = (uint16_t)v, .key = (uint8_t)k };
    __asm volatile ("" ::: "memory");
    kq_w = (uint8_t)(w + 1);
}

/* The oldest change not taken yet (0: none); key_take() drops it */
static int key_peek(struct key_change *c)
{
    if (kq_r == kq_w)
        return 0;
    __asm volatile ("" ::: "memory");
    *c = kq[kq_r % KQ];
    return 1;
}

static void key_take(void)
{
    kq_r = (uint8_t)(kq_r + 1);
}

static void key_log_pending(void)
{
    if (ign_ms) {
        log_printf("keys: %s held: %s for %u ms ignored", key_names[ign_key], key_names[ign_raw],
                   ign_ms);
        ign_ms = 0;
    }
    if (kq_lost) {
        log_printf("keys: %u changes lost (the queue full)", kq_lost);
        kq_lost = 0;
    }
}

/* ---- events: short press (on release), long press (held 600 ms), repeat for VOL+/VOL- ----
 * VOL+ and VOL- together (the chord): VOL- grounds the ladder, so the pair reads as VOL-. It
 * shows as a change from one volume key to the other with no release between them (held 100 ms,
 * keys_sample): EV_CHORD, *key the key pressed first, then nothing until both are released.
 * Since 0.53 a short press of VOL+/VOL- is also taken on release (a chord may follow it: no
 * volume step to undo); held, it repeats from 450 ms, without a step on release. The changes
 * are taken at their own times: a key held through a stall of the loop gives its long press or
 * its repeats at the times it was held, before its release. */
static int kp_key, kp_long, kp_rep, kp_chord;
static uint32_t kp_t0, kp_next;
static unsigned kp_adc;

int key_is_vol(int k)
{
    return k == KEY_VOLUP || k == KEY_VOLDOWN;
}

/* A change taken outside the events (key_edge): a key down is swallowed until its release */
static void key_set(const struct key_change *c)
{
    kp_key = c->key;
    kp_t0 = c->t;
    kp_adc = c->adc;
    kp_long = kp_rep = 0;
    kp_chord = c->key != KEY_NONE;
}

static int poll(int *key)
{
    struct key_change c;
    int have = key_peek(&c);
    uint32_t now = have ? c.t : ticks();

    if (kp_key && !kp_chord) {                  /* held up to now, or up to the next change */
        if (key_is_vol(kp_key)) {
            if ((int32_t)(now - kp_next) >= 0) {
                if (!kp_rep)
                    log_printf("keys: %s held (adc %u)", key_name(kp_key), kp_adc);
                kp_rep = 1;
                kp_next += 90000;
                *key = kp_key;
                return EV_REPEAT;
            }
        } else if (!kp_long && now - kp_t0 > 600000) {
            kp_long = 1;
            *key = kp_key;
            log_printf("keys: %s long (adc %u)", key_name(kp_key), kp_adc);
            return EV_LONG;
        }
    }
    if (!have)
        return EV_NONE;
    key_take();
    int k = c.key, old = kp_key, ev = EV_NONE;
    if (old && !kp_long && !kp_rep && !kp_chord && !(key_is_vol(old) && key_is_vol(k))) {
        *key = old;
        ev = EV_PRESS;
        log_printf("keys: %s (adc %u, %u ms)", key_name(old), kp_adc, (now - kp_t0) / 1000);
    }
    kp_key = k;
    kp_t0 = now;
    if (k)
        kp_adc = c.adc;
    kp_long = kp_rep = 0;
    if (!k)
        kp_chord = 0;
    if (kp_chord)
        return EV_NONE;
    if (key_is_vol(old) && key_is_vol(k)) {
        kp_chord = 1;
        *key = old;
        log_printf("keys: chord vol+- (%s first, adc %u)", key_name(old), c.adc);
        return EV_CHORD;
    }
    if (k && key_is_vol(k))
        kp_next = now + 450000;
    return ev;
}

/* ---- simple screens (system, installer): the key newly down, once ---- */
int key_edge(void)
{
    struct key_change c;

    key_log_pending();
    while (key_peek(&c)) {
        key_take();
        key_set(&c);
        if (c.key != KEY_NONE)
            return c.key;
    }
    return KEY_NONE;
}

void key_edge_sync(void)
{
    struct key_change c;

    while (key_peek(&c)) {
        key_take();
        key_set(&c);
    }
    kp_chord = kp_key != KEY_NONE;
}

/* ---- plugs: a change taken once it has lasted PLUG_SETTLE_US, read every 10 ms; the first
 * reading sets the state without an event ---- */
#define PLUG_SETTLE_US 100000u

static const char *const plug_names[] = { "jack 3.5 mm", "jack 4.4 mm", "usb cable" };
static int plug_in[3], plug_raw[3], plug_ready;
static uint32_t plug_since[3], plug_t;

static int plug_read(int i)
{
    return i == 0 ? gpio_get(PIN_JACK35) : i == 1 ? !gpio_get(PIN_JACK44) : vbus();
}

static int plug_poll(int *src)
{
    uint32_t now = ticks();
    if (plug_ready && now - plug_t < 10000)
        return EV_NONE;
    plug_t = now;
    for (int i = 0; i < 3; i++) {
        int v = plug_read(i);
        if (!plug_ready) {
            plug_in[i] = plug_raw[i] = v;
            continue;
        }
        if (v != plug_raw[i]) {
            plug_raw[i] = v;
            plug_since[i] = now;
        } else if (v != plug_in[i] && now - plug_since[i] >= PLUG_SETTLE_US) {
            plug_in[i] = v;
            *src = SRC_JACK35 + i;
            log_printf("keys: %s %s", plug_names[i], v ? "in" : "out");
            return v ? EV_PLUG : EV_UNPLUG;
        }
    }
    plug_ready = 1;
    return EV_NONE;
}

int plug_present(int src)
{
    return src >= SRC_JACK35 && src <= SRC_USB && plug_in[src - SRC_JACK35];
}

int key_poll(struct key_ev *e)
{
    key_log_pending();
    e->key = KEY_NONE;
    e->type = poll(&e->key);
    if (e->type == EV_NONE)
        e->type = plug_poll(&e->key);
    return e->type;
}

int key_held(void)
{
    return kp_key != KEY_NONE;
}

uint32_t key_held_us(void)
{
    return ticks() - kp_t0;
}
