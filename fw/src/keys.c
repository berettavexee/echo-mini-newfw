/* Keys: debouncing of the ladder, events for the screens (keys.h) */
#include <stdint.h>
#include "keys.h"
#include "board.h"
#include "log.h"

/* ---- debouncing ---- */
/* The ladder is RC filtered: pressing VOL-, VOL+ or Play sweeps channel 1 down through the
 * Menu range for a few ms. A key counts once its class has been the same for 30 ms. A key held
 * (0.53, ETUDE §34): a release counts after 60 ms (a loose contact of Play reads none for a few
 * ms; VOL+/VOL-: 30 ms, quick clicks leave 50-60 ms between them, 0.54), VOL+ <-> VOL- (the chord) after 100 ms, and another key not at all until a release (a loose
 * contact of Play sweeps up through Menu and Back: these read as Back presses in 0.52). */
#define KEY_SETTLE_US   30000
#define KEY_RELEASE_US  60000
#define KEY_CHORD_US   100000
static unsigned key_adc;                               /* channel 1 when the key held was taken */

static const char *const key_names[] = { "none", "vol-", "vol+", "play", "menu", "back" };

const char *key_name(int k)
{
    return key_names[k];
}

static int key_scan(void)
{
    static int raw = KEY_NONE, stable = KEY_NONE;
    static uint32_t since;
    unsigned v = adc_read(1);
    int k = key_ladder(v);
    uint32_t now = ticks();

    if (k != raw) {
        uint32_t held = now - since;
        if (raw != stable && stable != KEY_NONE && held >= KEY_SETTLE_US / 2)
            log_printf("keys: %s held: %s for %u ms ignored", key_names[stable], key_names[raw],
                       held / 1000);
        raw = k;
        since = now;
        return stable;
    }
    if (k == stable)
        return stable;
    uint32_t need = stable == KEY_NONE ? KEY_SETTLE_US :
                    k == KEY_NONE ? (stable == KEY_VOLUP || stable == KEY_VOLDOWN ?
                                     KEY_SETTLE_US : KEY_RELEASE_US) :
                    (stable == KEY_VOLUP || stable == KEY_VOLDOWN) &&
                    (k == KEY_VOLUP || k == KEY_VOLDOWN) ? KEY_CHORD_US : 0xffffffffu;
    if (now - since >= need) {
        stable = k;
        if (k != KEY_NONE)
            key_adc = v;
    }
    return stable;
}

/* ---- simple screens (system, installer): the key newly down, once ---- */
static int key_last;

int key_edge(void)
{
    int k = key_scan();
    if (k == key_last)
        return KEY_NONE;
    key_last = k;
    return k;
}

void key_edge_sync(void)
{
    key_last = key_scan();
}

/* ---- events: short press (on release), long press (held 600 ms), repeat for VOL+/VOL- ----
 * VOL+ and VOL- together (the chord): VOL- grounds the ladder, so the pair reads as VOL-. It
 * shows as a change from one volume key to the other with no release between them (held 100 ms,
 * key_scan): EV_CHORD, *key the key pressed first, then nothing until both are released. Since
 * 0.53 a short press of VOL+/VOL- is also taken on release (a chord may follow it: no volume step
 * to undo); held, it repeats from 450 ms, without a step on release. */
static int kp_key, kp_long, kp_rep, kp_chord;
static uint32_t kp_t0, kp_next;

int key_is_vol(int k)
{
    return k == KEY_VOLUP || k == KEY_VOLDOWN;
}

static int poll(int *key)
{
    int k = key_scan();
    uint32_t now = ticks();
    if (k != kp_key) {
        int old = kp_key, ev = EV_NONE;
        if (old && !kp_long && !kp_rep && !kp_chord && !(key_is_vol(old) && key_is_vol(k))) {
            *key = old;
            ev = EV_PRESS;
            log_printf("keys: %s (adc %u, %u ms)", key_name(old), key_adc, (now - kp_t0) / 1000);
        }
        kp_key = k;
        kp_t0 = now;
        kp_long = kp_rep = 0;
        if (!k)
            kp_chord = 0;
        if (kp_chord)
            return EV_NONE;
        if (key_is_vol(old) && key_is_vol(k)) {
            kp_chord = 1;
            *key = old;
            log_printf("keys: chord vol+- (%s first, adc %u)", key_name(old), key_adc);
            return EV_CHORD;
        }
        if (k && key_is_vol(k))
            kp_next = now + 450000;
        return ev;
    }
    if (!k || kp_chord)
        return EV_NONE;
    if (key_is_vol(k)) {
        if ((int32_t)(now - kp_next) >= 0) {
            if (!kp_rep)
                log_printf("keys: %s held (adc %u)", key_name(k), key_adc);
            kp_rep = 1;
            kp_next = now + 90000;
            *key = k;
            return EV_REPEAT;
        }
    } else if (!kp_long && now - kp_t0 > 600000) {
        kp_long = 1;
        *key = k;
        log_printf("keys: %s long (adc %u)", key_name(k), key_adc);
        return EV_LONG;
    }
    return EV_NONE;
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

int key_poll(struct key_ev *e)
{
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
