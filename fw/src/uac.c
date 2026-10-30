/* USB sound card (ETUDE §83, step 1 of the ROADMAP's plan): USB Audio Class 1 at full speed, 16
 * or 24 bits, 44.1 or 48 kHz, adaptive (no rate feedback yet). The host's packets go straight
 * into the DACs' ring of core A (audio.c), core B is not used. The host's clock and the DACs'
 * drift apart: the ring is kept about TARGET_MS ahead of the DMA by dropping or repeating one
 * frame per packet while it is more than WINDOW_MS off (until SETTLE_MS), and set again on
 * silence when it runs dry or overflows. The host's volume (Feature Unit) and our own (VOL+ and
 * VOL-) add up on the DACs' attenuation, in 0.5 dB steps. Polled: the RX FIFO holds about 3
 * packets, so the loop must come back to usb_service() within 3 ms (the longest gap is logged). */
#include "ui_priv.h"
#include "sys_priv.h"
#include "usb.h"

#define TARGET_MS   10
#define WINDOW_US   3000                        /* drift corrected beyond this */
#define SETTLE_US   500                         /* until back within this */
#define AHEAD_MS    20                          /* silence written after the last packet */
#define STALL_MS    50                          /* no packet for so long: the stream stopped */
#define MPS16       196                         /* 49 frames of 4 bytes (44.1 kHz: 44 or 45) */
#define MPS24       294                         /* 49 frames of 6 bytes */

/* ---- descriptors: test IDs of pid.codes (private use only), bcdDevice 2.00 for this class ---- */
static const uint8_t dev_desc[18] = {
    18, 1, 0x10, 0x01, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x02, 1, 2, 3, 1,
};

#define FORMAT(alt, bytes, bits, mps)                                                          \
    9, 4, 1, alt, 1, 1, 2, 0, 0,                        /* streaming interface, alternate */   \
    7, 0x24, 1, 1, 1, 0x01, 0x00,                       /* general: terminal 1, PCM */          \
    14, 0x24, 2, 1, 2, bytes, bits, 2, 0x44, 0xac, 0x00, 0x80, 0xbb, 0x00,  /* 44.1, 48 kHz */  \
    9, 5, 0x01, 0x09, (mps) & 0xff, (mps) >> 8, 1, 0, 0, /* OUT 1, isochronous, adaptive */      \
    7, 0x25, 1, 0x01, 0, 0, 0                           /* sampling frequency control */

static const uint8_t cfg_desc[] = {
    9, 2, 159, 0, 2, 1, 0, 0x80, 250,                   /* 2 interfaces, bus powered, 500 mA */
    9, 4, 0, 0, 0, 1, 1, 0, 0,                          /* audio control */
    9, 0x24, 1, 0x00, 0x01, 40, 0, 1, 1,                /* header: UAC 1.00, streaming if 1 */
    12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0, /* input terminal 1: USB, L and R */
    10, 0x24, 6, 2, 1, 1, 0x03, 0, 0, 0,                /* feature unit 2: mute, volume */
    9, 0x24, 3, 3, 0x02, 0x03, 0, 2, 0,                 /* output terminal 3: headphones */
    9, 4, 1, 0, 0, 1, 2, 0, 0,                          /* streaming, alternate 0: no data */
    FORMAT(1, 2, 16, MPS16),
    FORMAT(2, 3, 24, MPS24),
};

/* ---- the host's side ---- */
static volatile int alt, alt_new = -1;          /* streaming alternate: 0 off, 1 16-bit, 2 24-bit */
static volatile uint32_t rate = 48000, rate_new;
static volatile int16_t host_vol;               /* 1/256 dB, -64 dB to 0 */
static volatile int host_mute, vol_changed = 1;

#define VOL_MIN (-64 * 256)

static int put16(uint8_t *r, int v)
{
    r[0] = (uint8_t)v;
    r[1] = (uint8_t)(v >> 8);
    return 2;
}

static int uac_setup(const uint8_t *s, uint8_t *r)
{
    uint8_t type = s[0], req = s[1], cs = s[3];
    uint16_t index = s[4] | s[5] << 8, value = s[2] | s[3] << 8;
    if ((type & 0x60) == 0) {                           /* SET_INTERFACE, GET_INTERFACE */
        if ((index & 0xff) == 0)
            return req == 10 ? (r[0] = 0, 1) : 0;
        if (req == 10) {
            r[0] = (uint8_t)alt;
            return 1;
        }
        if (value > 2)
            return -1;
        if (value)
            usb_iso_out_open(value == 1 ? MPS16 : MPS24);
        else
            usb_iso_out_close();
        alt = value;
        alt_new = value;
        return 0;
    }
    if ((type & 0x1f) == 2) {                           /* endpoint: the sampling frequency */
        if ((index & 0x0f) != 1 || cs != 1 || req != 0x81)
            return -1;
        r[0] = (uint8_t)rate;
        r[1] = (uint8_t)(rate >> 8);
        r[2] = (uint8_t)(rate >> 16);
        return 3;
    }
    if (index != 0x0200 || (s[2] != 0))                 /* feature unit 2, master channel */
        return -1;
    if (cs == 1 && req == 0x81) {
        r[0] = (uint8_t)host_mute;
        return 1;
    }
    if (cs != 2)
        return -1;
    switch (req) {
    case 0x81: return put16(r, host_vol);               /* GET_CUR */
    case 0x82: return put16(r, VOL_MIN);                /* GET_MIN */
    case 0x83: return put16(r, 0);                      /* GET_MAX */
    case 0x84: return put16(r, 128);                    /* GET_RES: 0.5 dB */
    }
    return -1;
}

static void uac_setup_out(const uint8_t *s, const uint8_t *d, uint32_t n)
{
    uint8_t req = s[1], cs = s[3];
    uint16_t index = s[4] | s[5] << 8;
    if (req != 0x01)                                    /* SET_CUR only */
        return;
    if ((s[0] & 0x1f) != 2 && index != 0x0200)          /* not feature unit 2 */
        return;
    if ((s[0] & 0x1f) == 2 && cs == 1 && n >= 3) {     /* sampling frequency */
        uint32_t f = d[0] | d[1] << 8 | (uint32_t)d[2] << 16;
        if (f == 44100 || f == 48000) {
            rate_new = f;
            if (alt)
                alt_new = alt;                          /* applied with the format */
        }
    } else if (cs == 1 && n >= 1) {
        host_mute = d[0] != 0;
        vol_changed = 1;
    } else if (cs == 2 && n >= 2) {
        int v = (int16_t)(d[0] | d[1] << 8);
        host_vol = (int16_t)(v < VOL_MIN ? VOL_MIN : v > 0 ? 0 : v);
        vol_changed = 1;
    }
}

/* ---- the stream into the ring ---- */
static struct {
    int ready;                  /* the output runs at the host's format: packets go to the ring */
    int flowing;                /* packets came lately (else: silence) */
    int corr;                   /* drift correction: -1 drop, +1 repeat, 0 none */
    uint32_t frame;             /* bytes of a frame in the ring: 4 (16 bits) or 8 (24 bits) */
    int32_t target, window, settle, ahead;  /* in ring bytes */
    uint32_t last_ms;           /* millis() of the last packet */
    uint32_t packets, dropped, repeated, realigned, bad;
    int32_t fill_min, fill_max;
} st;
static uint32_t rate_rd0, rate_t0;                      /* the output's real rate, from the DMA */
static uint32_t pkt[(MPS24 + 3) / 4], out[2 * 49 + 2];

static int32_t ms_bytes(uint32_t us)
{
    return (int32_t)((uint64_t)rate * st.frame * us / 1000000) & ~3;
}

static void uac_iso_out(uint32_t n)
{
    uint32_t words = (n + 3) / 4;
    if (words > sizeof pkt / 4) {                       /* larger than the endpoint: dropped */
        for (uint32_t i = 0; i < words; i++)
            (void)usb_rx_word();
        st.bad++;
        return;
    }
    for (uint32_t i = 0; i < words; i++)
        pkt[i] = usb_rx_word();
    st.last_ms = millis();
    if (!st.ready || !n)
        return;
    st.packets++;
    /* to the ring's format: 16 bits as they are (left low, right high), 24 bits in the high
     * bits of a 32-bit word per sample */
    uint32_t frames, k = 0;
    if (alt == 1) {
        frames = n / 4;
        for (uint32_t i = 0; i < frames; i++)
            out[k++] = pkt[i];
    } else {
        const uint8_t *b = (const uint8_t *)pkt;
        frames = n / 6;
        for (uint32_t i = 0; i < frames; i++, b += 6) {
            out[k++] = (uint32_t)b[0] << 8 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 24;
            out[k++] = (uint32_t)b[3] << 8 | (uint32_t)b[4] << 16 | (uint32_t)b[5] << 24;
        }
    }
    if (!frames)
        return;
    audio_update();
    int32_t fill = audio_fill();
    if (!st.flowing || fill < st.target / 4 || fill > 4 * st.target) {
        if (st.flowing)
            st.realigned++;
        audio_stream_align((uint32_t)st.target);
        st.flowing = 1;
        st.corr = 0;
        fill = audio_fill();
    }
    if (fill < st.fill_min)
        st.fill_min = fill;
    if (fill > st.fill_max)
        st.fill_max = fill;
    int32_t err = fill - st.target;
    if (!st.corr && err > st.window)
        st.corr = -1;
    else if (!st.corr && err < -st.window)
        st.corr = 1;
    else if ((st.corr < 0 && err <= st.settle) || (st.corr > 0 && err >= -st.settle))
        st.corr = 0;
    uint32_t wpf = st.frame / 4;                        /* words of a frame */
    if (st.corr < 0 && frames > 1) {                    /* the DACs are slower: one less */
        k -= wpf;
        st.dropped++;
    } else if (st.corr > 0) {                           /* faster: the last frame twice */
        for (uint32_t i = 0; i < wpf; i++, k++)
            out[k] = out[k - wpf];
        st.repeated++;
    }
    audio_write(out, 4 * k);
    audio_zero_ahead((uint32_t)st.ahead);
}

static void uac_reset(void)
{
    alt = 0;
    alt_new = 0;
}

static const struct usb_class uac = {
    dev_desc, cfg_desc, sizeof cfg_desc, 1, 276, uac_setup, uac_setup_out, uac_iso_out, uac_reset,
};

/* The output at the host's format: the rate and the ring's frame (16 or 32-bit words) */
static uint32_t cur_rate;                               /* the output's format now */
static int cur_alt;

static void apply_format(void)
{
    int a = alt_new;
    alt_new = -1;
    if (rate_new) {
        rate = rate_new;
        rate_new = 0;
    }
    st.ready = 0;
    st.flowing = 0;
    if (a <= 0) {
        log_printf("uac: stream off");
        return;
    }
    int bits16 = a == 1;
    if ((rate != cur_rate || a != cur_alt) && audio_change_rate(rate, bits16) &&
        audio_start(rate, bits16))
        log_printf("uac: output not started");
    cur_rate = rate;
    cur_alt = a;
    st.frame = bits16 ? 4 : 8;
    st.target = ms_bytes(TARGET_MS * 1000);
    st.window = ms_bytes(WINDOW_US);
    st.settle = ms_bytes(SETTLE_US);
    st.ahead = ms_bytes(AHEAD_MS * 1000);
    st.fill_min = 0x7fffffff;
    st.fill_max = 0;
    audio_stream_align((uint32_t)st.target);
    rate_rd0 = audio.rd;
    rate_t0 = ticks();
    vol_changed = 1;
    st.ready = 1;
    log_printf("uac: stream %u Hz, %u bits", rate, bits16 ? 16 : 24);
}

/* ---- the screen ---- */
static int level;                               /* our own attenuation, 0.5 dB steps */

static void screen(int line, uint32_t max_gap)
{
    char b[64], *p;
    switch (line) {
    case 0:
    p = put_str(b, !usb.configured ? "waiting for the host" : !alt ? "connected, no stream" :
                   st.flowing ? "playing" : "stream open, no sound");
    *p = 0;
    sys_status_line(0, 36, b, usb.configured ? FG : DIM);
    break;
    case 1:
    p = b;
    if (alt) {
        p = put_dec(p, rate / 1000);
        p = put_str(p, rate % 1000 ? ".1 kHz, " : " kHz, ");
        p = put_dec(p, alt == 1 ? 16 : 24);
        p = put_str(p, " bits");
    }
    *p = 0;
    sys_status_line(1, 52, b, FG);
    break;
    case 2:
    p = put_str(b, "volume -");
    p = put_dec(p, (uint32_t)level / 2);
    p = put_str(p, level & 1 ? ".5 dB, host -" : " dB, host -");
    p = put_dec(p, (uint32_t)(-host_vol) / 256);
    p = put_str(p, " dB");
    if (host_mute)
        p = put_str(p, ", muted");
    *p = 0;
    sys_status_line(2, 68, b, FG);
    break;
    case 3:
    p = put_str(b, "fill ");
    p = put_dec(p, st.frame && rate ? (uint32_t)(audio_fill() > 0 ? audio_fill() : 0) * 1000 /
                                          (rate * st.frame) : 0);
    p = put_str(p, " ms, dropped ");
    p = put_dec(p, st.dropped);
    p = put_str(p, ", repeated ");
    p = put_dec(p, st.repeated);
    *p = 0;
    sys_status_line(3, 84, b, DIM);
    break;
    default:
    p = put_str(b, "realigned ");
    p = put_dec(p, st.realigned);
    p = put_str(p, ", missed ");
    p = put_dec(p, usb.iso_incomplete);
    p = put_str(p, ", gap ");
    p = put_dec(p, max_gap / 1000);
    p = put_str(p, " ms");
    *p = 0;
    sys_status_line(4, 100, b, st.realigned || usb.iso_incomplete ? WARN : DIM);
    }
}

/* The longest time of each part of the loop, us: USB, format, volume, keys, screen, log, idle */
enum { T_USB, T_FORMAT, T_VOLUME, T_KEYS, T_SCREEN, T_LOG, T_IDLE, T_N };
static uint32_t t_max[T_N];

static void timed(int part, uint32_t since)
{
    uint32_t d = ticks() - since;
    if (d > t_max[part])
        t_max[part] = d;
}

static void log_stats(const char *what, uint32_t max_gap)
{
    log_printf("uac: %s: %u packets, %u dropped, %u repeated, %u realigned, %u missed, %u bad, "
               "fill %d..%d bytes (target %d), ring underruns %u, longest loop gap %u us", what,
               st.packets, st.dropped, st.repeated, st.realigned, usb.iso_incomplete, st.bad,
               st.fill_min == 0x7fffffff ? 0 : st.fill_min, st.fill_max, st.target, audio.ring_ur,
               max_gap);
    audio_update();
    uint32_t dt = ticks() - rate_t0;
    if (rate_t0 && dt > 1000000 && st.frame)
        log_printf("uac: output rate %u.%u Hz over %u ms (nominal %u)",
                   (uint32_t)((uint64_t)(audio.rd - rate_rd0) * 10000000 / st.frame / dt) / 10,
                   (uint32_t)((uint64_t)(audio.rd - rate_rd0) * 10000000 / st.frame / dt) % 10,
                   dt / 1000, rate);
    rate_rd0 = audio.rd;
    rate_t0 = ticks();
    log_printf("uac: longest parts, us: usb %u, format %u, volume %u, keys %u, screen line %u, "
               "log %u, idle %u", t_max[T_USB], t_max[T_FORMAT], t_max[T_VOLUME], t_max[T_KEYS],
               t_max[T_SCREEN], t_max[T_LOG], t_max[T_IDLE]);
    for (int i = 0; i < T_N; i++)
        t_max[i] = 0;
}

void uac_mode(void)
{
    sys_status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    sys_line(0, "USB sound card", TITLE);
    if (!vbus()) {
        sys_line(36, "plug the USB cable", WARN);
        mdelay(2000);
        return;
    }
    sys_line(138, "VOL+ / VOL-: volume", DIM);
    sys_line(154, "BACK held: exit", DIM);
    level = play_level;
    rate = 48000;
    alt = 0;
    alt_new = -1;
    rate_new = 0;
    host_vol = 0;
    host_mute = 0;
    st = (typeof(st)){ 0 };
    st.frame = 4;
    audio_stop();                                       /* from scratch: not parked, not paused */
    if (audio_start(rate, 1)) {
        sys_line(36, "audio output failed", WARN);
        mdelay(2000);
        return;
    }
    audio_volume(0xff, 0);
    cur_rate = rate;
    cur_alt = 1;
    log_printf("uac: start");
    usb_start_class(&uac);
    lcd_hook = usb_service;                             /* packets served while drawing */
    gpio_set(2, 0, 0);          /* usb_start sets them as RECHO's disk mode; the DACs (audio.c) */
    gpio_set(1, 4, 1);          /* want GPIO2_0 low (high mutes the jacks) and GPIO1_4 high */
    uint32_t t0 = millis(), t_draw = 0, t_key = 0, t_log = millis(), last = ticks(), max_gap = 0;
    uint32_t gap_all = 0, configured_at = 0;
    const char *why = "?";
    for (;;) {
        wdt_kick();
        uint32_t now = ticks();
        if (now - last > max_gap)
            max_gap = now - last;
        last = now;
        usb_service();
        timed(T_USB, now);
        if (usb.configured && !configured_at) {
            configured_at = millis();
            log_printf("uac: configured after %u ms", configured_at - t0);
        }
        if (alt_new >= 0) {
            uint32_t t = ticks();
            apply_format();
            timed(T_FORMAT, t);
        }
        if (st.ready && st.flowing && millis() - st.last_ms > STALL_MS) {
            st.flowing = 0;                             /* the host stopped sending */
            log_stats("paused", max_gap);
        }
        if (!st.flowing) {
            uint32_t t = ticks();
            audio_idle();
            timed(T_IDLE, t);
        }
        if (vol_changed) {
            uint32_t t = ticks();
            vol_changed = 0;
            int a = level + (-host_vol) / 128;
            audio_volume(host_mute ? 0xff : (uint8_t)(a > 0xfe ? 0xfe : a), 0);
            timed(T_VOLUME, t);
        }
        if (now - t_key > 10000) {
            t_key = now;
            if (!vbus()) {
                why = "cable unplugged";
                break;
            }
            struct key_ev e;
            int ev = key_poll(&e);
            timed(T_KEYS, now);
            if ((ev == EV_PRESS || ev == EV_REPEAT) && key_is_vol(e.key)) {
                int d = (e.key == KEY_VOLUP) != !!cfg.swap_vol ? -2 : 2;
                level = level + d < 0 ? 0 : level + d > 0xfe ? 0xfe : level + d;
                vol_changed = 1;
            } else if (ev == EV_LONG && e.key == KEY_BACK) {
                why = "BACK";
                break;
            }
        }
        if (now - t_draw > 100000) {                    /* a line every 100 ms */
            static int line;
            t_draw = now;
            uint32_t t = ticks();
            screen(line, max_gap);
            timed(T_SCREEN, t);
            line = (line + 1) % 5;
        }
        if (st.flowing && millis() - t_log > 10000) {
            t_log = millis();
            log_stats("playing", max_gap);
            if (max_gap > gap_all)
                gap_all = max_gap;
            max_gap = 0;                                /* the longest gap per 10 s */
        } else if (!st.flowing) {
            uint32_t t = ticks();
            log_poll();                                 /* the log's writes: not while playing */
            timed(T_LOG, t);
        }
    }
    if (max_gap > gap_all)
        gap_all = max_gap;
    lcd_hook = 0;
    usb_iso_out_close();
    usb_stop();
    audio_volume(0xff, 0);
    audio_stop();
    log_stats(why, gap_all);
    log_printf("uac: exit (%s) after %u s", why, (millis() - t0) / 1000);
}
