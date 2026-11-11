/* USB sound card (ETUDE §83-85, steps 1 and 2 of the ROADMAP's plan): USB Audio Class 1 at full
 * speed, 16 or 24 bits, 44.1 or 48 kHz, asynchronous. The host's packets go straight into the
 * DACs' ring of core A (audio.c), core B is not used. The DACs keep their own clock; a feedback
 * endpoint tells the host how many samples to send per frame (10.14), from the ring's fill: a
 * proportional-integral control towards TARGET_MS (the integral finds the clocks' difference,
 * some 40 ppm). Should the fill still go more than WINDOW_US off, one frame per
 * packet is dropped or repeated (until SETTLE_US); the ring is set again on silence when it runs
 * dry or overflows. The host's volume (Feature Unit) and our own (VOL+ and
 * VOL-, kept in the settings, the playback volume until then) add up on the DACs' attenuation,
 * in 0.5 dB steps. The USB runs from the controller's interrupt (usb_irq): the packets go into the
 * ring there; what the loop shares with it (the ring when no packet comes, the format, the DMA's
 * position) is done with the interrupt blocked. */
#include "ovl.h"
#include "ui_priv.h"
#include "sys_priv.h"
#include "usb.h"
#include "remote.h"

#define TARGET_MS   10
#define WINDOW_US   4000                        /* drift corrected beyond this (a fallback) */
#define SETTLE_US   500                         /* until back within this */
#define AHEAD_MS    20                          /* silence written after the last packet */
#define STALL_MS    50                          /* no packet for so long: the stream stopped */
#define MPS16       196                         /* 49 frames of 4 bytes (44.1 kHz: 44 or 45) */
#define MPS24       294                         /* 49 frames of 6 bytes */
/* The control, per packet (1 ms): the fill averaged over about 64 packets (frames x 16); 10.14
 * units of feedback: FB_KP per frame of error, and the integral's 1/256 per frame of error per
 * packet. The fill then follows (feedback - the DACs' rate) / 16384 frames per ms: about 0.5 of
 * damping, settled in a few seconds. */
#define FB_KP       8
/* A host that ignores the feedback (phones: ETUDE §90): the control stays at its 1 % limit for
 * IGNORED_MS; the feedback then goes back to the nominal, and the drift is corrected one frame
 * at a time, whenever the averaged fill is more than one frame off, SPREAD_MS apart at least
 * (about one every 0.4 s at 50 ppm), instead of bursts at the WINDOW_US fallback */
#define IGNORED_MS  10000
#define SPREAD_MS   100

/* ---- descriptors: test IDs of pid.codes (private use only), bcdDevice 2.00 for this class ---- */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x02, 1, 2, 3, 1,
};

#define FORMAT(alt, bytes, bits, mps)                                                          \
    9, 4, 1, alt, 2, 1, 2, 0, 0,                        /* streaming, alternate, 2 endpoints */ \
    7, 0x24, 1, 1, 1, 0x01, 0x00,                       /* general: terminal 1, PCM */          \
    14, 0x24, 2, 1, 2, bytes, bits, 2, 0x44, 0xac, 0x00, 0x80, 0xbb, 0x00,  /* 44.1, 48 kHz */  \
    9, 5, 0x01, 0x05, (mps) & 0xff, (mps) >> 8, 1, 0, 0x81, /* OUT 1, asynchronous, sync 0x81 */ \
    7, 0x25, 1, 0x01, 0, 0, 0,                          /* sampling frequency control */        \
    9, 5, 0x81, 0x01, 3, 0, 1, 1, 0                     /* IN 1: feedback, 3 bytes, 2 ms */

static const uint8_t cfg_desc[] = {
    9, 2, 177, 0, 2, 1, 0, 0x80, 250,                   /* 2 interfaces, bus powered, 500 mA */
    9, 4, 0, 0, 0, 1, 1, 0, 0,                          /* audio control */
    9, 0x24, 1, 0x00, 0x01, 40, 0, 1, 1,                /* header: UAC 1.00, streaming if 1 */
    12, 0x24, 2, 1, 0x01, 0x01, 0, 2, 0x03, 0x00, 0, 0, /* input terminal 1: USB, L and R */
    10, 0x24, 6, 2, 1, 1, 0x03, 0, 0, 0,                /* feature unit 2: mute, volume */
    9, 0x24, 3, 3, 0x02, 0x03, 0, 2, 0,                 /* output terminal 3: headphones */
    9, 4, 1, 0, 0, 1, 2, 0, 0,                          /* streaming, alternate 0: no data */
    FORMAT(1, 2, 16, MPS16),
    FORMAT(2, 3, 24, MPS24),
};

/* ---- at high speed: USB Audio Class 2 (an interface association, class 0xEF), every rate the
 * DACs play from 44.1 to 192 kHz, 16 bits, 24 bits in 4 bytes (the ring's format), 32 bits; a
 * packet every 4 microframes (500 us) in 16 bits, every 2 (250 us) in 24 and 32 bits (a packet
 * every microframe, re-armed from the interrupt, missed one in two in 97Q), the feedback (16.16,
 * samples per microframe) every 8 (1 ms) ---- */
static const uint8_t dev_hs[18] = {
    18, 1, 0x00, 0x02, 0xef, 0x02, 0x01, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x02, 1, 2, 3, 1,
};

/* The RX FIFO is fixed at 147 words (hwcfg2: no dynamic FIFO sizing, GRXFSIZ stays): a packet
 * and its status word must fit, 392 bytes at most here. At 192 kHz that is 500 us of 16-bit
 * frames, but only 250 us of 8-byte frames (776 bytes in 500 us, dropped by the controller) */
#define MPS2_16     392                         /* 98 frames of 4 bytes (192 kHz: 96, 500 us) */
#define MPS2_32     392                         /* 49 frames of 8 bytes (192 kHz: 48, 250 us) */
#define CLOCK_ID    0x10
static const uint32_t rates2[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
#define N_RATES2 (int)(sizeof rates2 / sizeof *rates2)

#define FORMAT2(alt, sub, bits, mps, interval)                                                 \
    9, 4, 1, alt, 2, 1, 2, 0x20, 0,                     /* streaming, alternate, 2 endpoints */ \
    16, 0x24, 1, 1, 0, 1, 1, 0, 0, 0, 2, 3, 0, 0, 0, 0, /* general: terminal 1, PCM, L and R */ \
    6, 0x24, 2, 1, sub, bits,                           /* type I: subslot, bits */             \
    7, 5, 0x01, 0x05, (mps) & 0xff, (mps) >> 8, interval,   /* OUT 1, asynchronous */           \
    8, 0x25, 1, 0, 0, 0, 0, 0,                                                                   \
    7, 5, 0x81, 0x11, 4, 0, 4                           /* IN 1: feedback, 1 ms */

/* High speed: a HID interface (2) beside the sound card, the headset remote (remote.h), whose
 * interrupt IN endpoint 2 runs on the non-periodic TX FIFO (the core has one periodic IN endpoint,
 * the feedback's): consumer control, a bit per key (RC_*), validated on the device with 0.99uh */
#define CFG_HS_LEN  283
static const uint8_t hid_report[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,                 /* consumer page, consumer control */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03,     /* three bits */
    0x09, 0xcd, 0x09, 0xb5, 0x09, 0xb6, 0x81, 0x02,     /* play/pause, next, previous */
    0x95, 0x05, 0x81, 0x01, 0xc0,                       /* 5 bits of padding */
};

static const uint8_t cfg_hs[] = {
    9, 2, CFG_HS_LEN & 0xff, CFG_HS_LEN >> 8, 3, 1, 0, 0x80, 250,  /* 3 interfaces, 500 mA */
    8, 0x0b, 0, 2, 1, 0, 0x20, 0,                       /* association: audio, interfaces 0-1 */
    9, 4, 0, 0, 0, 1, 1, 0x20, 0,                       /* audio control, UAC 2 */
    9, 0x24, 1, 0x00, 0x02, 0x01, 64, 0, 0,             /* header: 2.00, desktop speaker */
    8, 0x24, 0x0a, CLOCK_ID, 0x03, 0x07, 0, 0,          /* clock: internal, programmable */
    17, 0x24, 2, 1, 0x01, 0x01, 0, CLOCK_ID, 2, 3, 0, 0, 0, 0, 0, 0, 0,     /* input: USB */
    18, 0x24, 6, 2, 1, 0x0f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* feature: mute, volume */
    12, 0x24, 3, 3, 0x02, 0x03, 0, 2, CLOCK_ID, 0, 0, 0,          /* output: headphones */
    9, 4, 1, 0, 0, 1, 2, 0x20, 0,                       /* streaming, alternate 0: no data */
    FORMAT2(1, 2, 16, MPS2_16, 3),                      /* 500 us */
    FORMAT2(2, 4, 24, MPS2_32, 2),                      /* 250 us */
    FORMAT2(3, 4, 32, MPS2_32, 2),
    9, 4, 2, 0, 1, 3, 0, 0, 0,                          /* HID, no boot protocol */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof hid_report, 0,         /* HID 1.11, its report */
    7, 5, 0x82, 0x03, 8, 0, 8,                          /* IN 2: interrupt, 16 ms */
};
_Static_assert(sizeof cfg_hs == CFG_HS_LEN, "UAC2 configuration length");

/* ---- the host's side ---- */
static volatile int alt, alt_new = -1;          /* streaming alternate: 0 off, 1 16-bit, 2 24-bit */
static volatile uint32_t rate = 48000, rate_new;
static volatile int16_t host_vol;               /* 1/256 dB, -64 dB to 0 */
static volatile int host_mute, vol_changed = 1;

#define VOL_MIN (-64 * 256)

/* ---- the feedback: samples per frame, 10.14 (at high speed, sent as 16.16 per microframe:
 * half the value) ---- */
static struct {
    int32_t avg;                /* the fill, frames x 16, averaged */
    int32_t integ;              /* the integral, 1/256 of the 10.14 unit */
    int32_t err;                /* the last error, frames (the target minus the average) */
    uint32_t sent;              /* the last value sent */
    int on;                     /* the control runs (packets flowing) */
    uint32_t at_limit;          /* ms the integral has stayed at its limit */
    volatile int ignored;       /* the host ignores the feedback (until the stream restarts) */
} fb;

static uint32_t fb_nominal(uint32_t hz)
{
    return (uint32_t)(((uint64_t)hz << 14) / 1000);
}

static uint32_t fb_value(void);
static void fb_control(int32_t fill);

#ifdef ECHO_UACDIAG
/* Test build (make EXTRA=-DECHO_UACDIAG): what the host does with the stream. The frames it sends
 * (its real rate, the frames per packet), each channel's peak, its control requests (and the
 * ones stalled), and feedback steps after 20 s of stream, 20 s each: +0.5 %, -0.5 % and
 * +0.05 % in 16.16 per microframe (the format of USB 2.0 at high speed), then +0.5 % in 16.16
 * per ms and in 10.14 per ms (formats a host might read instead), then the control again; a
 * host that follows the feedback changes its rate with it (ETUDE §90, docs/USB_AUDIO.md). A
 * 3-byte packet at high speed made an iPhone stop the stream: not tried any more. */
#define DIAG_SIZES  6
#define DIAG_REQS   64
static struct {
    uint32_t frames;                            /* frames received, before any drop or repeat */
    uint16_t size[DIAG_SIZES];                  /* frames per packet seen, and their counts */
    uint32_t count[DIAG_SIZES], other;
    uint32_t peak_l, peak_r;                    /* of 32767 */
    volatile int step;                          /* the feedback step forced (1-5), or 0 */
    uint32_t t0;                                /* millis() when the stream began to flow */
} diag;
static struct diag_req {
    uint8_t s[8], d[4], n, stalled;
} diag_req[DIAG_REQS];
static volatile uint32_t diag_head, diag_tail, diag_lost;

static void diag_record(const uint8_t *s, const uint8_t *d, uint32_t n, int stalled)
{
    if (diag_head - diag_tail >= DIAG_REQS) {
        diag_lost++;
        return;
    }
    struct diag_req *q = &diag_req[diag_head % DIAG_REQS];
    for (int i = 0; i < 8; i++)
        q->s[i] = s[i];
    for (uint32_t i = 0; i < 4; i++)
        q->d[i] = d && i < n ? d[i] : 0;
    q->n = (uint8_t)n;
    q->stalled = (uint8_t)stalled;
    diag_head++;
}

/* From the loop: the requests recorded by the interrupt */
static void diag_drain(void)
{
    while (diag_tail != diag_head) {
        const struct diag_req *q = &diag_req[diag_tail % DIAG_REQS];
        log_printf("uac: req %02x %02x value %04x index %04x length %u%s: %02x %02x %02x %02x%s",
                   q->s[0], q->s[1], q->s[2] | q->s[3] << 8, q->s[4] | q->s[5] << 8,
                   q->s[6] | q->s[7] << 8, q->s[0] & 0x80 ? " (in)" : "", q->d[0], q->d[1],
                   q->d[2], q->d[3], q->stalled ? ", stalled" : "");
        diag_tail++;
    }
    if (diag_lost) {
        log_printf("uac: req: %u not recorded", diag_lost);
        diag_lost = 0;
    }
}

static void diag_packet(const uint32_t *out, uint32_t frames, int bits16)
{
    diag.frames += frames;
    int i = 0;
    while (i < DIAG_SIZES && diag.count[i] && diag.size[i] != frames)
        i++;
    if (i < DIAG_SIZES) {
        diag.size[i] = (uint16_t)frames;
        diag.count[i]++;
    } else
        diag.other++;
    for (uint32_t f = 0; f < frames; f++) {
        int32_t l, r;
        if (bits16) {
            l = (int16_t)out[f];
            r = (int16_t)(out[f] >> 16);
        } else {
            l = (int32_t)out[2 * f] >> 16;
            r = (int32_t)out[2 * f + 1] >> 16;
        }
        uint32_t al = (uint32_t)(l < 0 ? -l : l), ar = (uint32_t)(r < 0 ? -r : r);
        if (al > diag.peak_l)
            diag.peak_l = al;
        if (ar > diag.peak_r)
            diag.peak_r = ar;
    }
}

/* The period's figures, then a new period; dt in us */
static void diag_log(uint32_t dt)
{
    char b[80], *p = b;
    for (int i = 0; i < DIAG_SIZES && diag.count[i]; i++) {
        p = put_dec(p, diag.size[i]);
        *p++ = ':';
        p = put_dec(p, diag.count[i]);
        *p++ = ' ';
    }
    *p = 0;
    uint32_t hz10 = dt ? (uint32_t)((uint64_t)diag.frames * 10000000 / dt) : 0;
    if (dt)
        log_printf("uac: host %u.%u Hz over %u ms; frames per packet %s(other %u); peaks L %u R %u; "
               "feedback step %d", hz10 / 10, hz10 % 10, dt / 1000, b, diag.other, diag.peak_l,
               diag.peak_r, diag.step);
    diag.frames = diag.other = diag.peak_l = diag.peak_r = 0;
    for (int i = 0; i < DIAG_SIZES; i++)
        diag.count[i] = 0;
}
#endif

static void fb_send(void)
{
    uint8_t b[4];
    fb.sent = fb_value();
    uint32_t v = usb.high_speed ? fb.sent >> 1 : fb.sent, n = usb.high_speed ? 4 : 3;
#ifdef ECHO_UACDIAG
    if (usb.high_speed && diag.step >= 4)       /* the step in other formats (10.14 per ms) */
        v = diag.step == 4 ? fb.sent << 2 : fb.sent;
#endif
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    usb_iso_in_send(b, n);
}

static int put32(uint8_t *r, uint32_t v)
{
    r[0] = (uint8_t)v;
    r[1] = (uint8_t)(v >> 8);
    r[2] = (uint8_t)(v >> 16);
    r[3] = (uint8_t)(v >> 24);
    return 4;
}

static int put16(uint8_t *r, int v)
{
    r[0] = (uint8_t)v;
    r[1] = (uint8_t)(v >> 8);
    return 2;
}

/* UAC2 requests: CUR (1) and RANGE (2) of the clock (its rate, valid) and of the feature unit
 * (mute, volume), the entity in the high byte of wIndex */
static int uac2_get(uint8_t req, uint8_t entity, uint8_t cs, uint8_t cn, uint8_t *r)
{
    if (entity == CLOCK_ID && cs == 1 && req == 1)
        return put32(r, rate);
    if (entity == CLOCK_ID && cs == 1 && req == 2) {    /* the rates: one range each */
        int n = put16(r, N_RATES2);
        for (int i = 0; i < N_RATES2; i++) {
            n += put32(r + n, rates2[i]);
            n += put32(r + n, rates2[i]);
            n += put32(r + n, 0);
        }
        return n;
    }
    if (entity == CLOCK_ID && cs == 2 && req == 1) {    /* valid */
        r[0] = 1;
        return 1;
    }
    if (entity != 2 || cn != 0)
        return -1;
    if (cs == 1 && req == 1) {
        r[0] = (uint8_t)host_mute;
        return 1;
    }
    if (cs == 2 && req == 1)
        return put16(r, host_vol);
    if (cs == 2 && req == 2) {                          /* one range: -64 to 0 dB by 0.5 */
        int n = put16(r, 1);
        n += put16(r + n, VOL_MIN);
        n += put16(r + n, 0);
        n += put16(r + n, 128);
        return n;
    }
    return -1;
}

static int uac_setup(const uint8_t *s, uint8_t *r)
{
    uint8_t type = s[0], req = s[1], cs = s[3];
    uint16_t index = s[4] | s[5] << 8, value = s[2] | s[3] << 8;
    int hs = usb.high_speed;
    if ((index & 0xff) == 2 && (type & 0x1f) == 1)     /* the HID: SET_IDLE accepted */
        return (type & 0x60) == 0 ? (req == 10 ? (r[0] = 0, 1) : 0) : req == 0x0a ? 0 : -1;
    if ((type & 0x60) == 0) {                           /* SET_INTERFACE, GET_INTERFACE */
        if ((index & 0xff) == 0)
            return req == 10 ? (r[0] = 0, 1) : 0;
        if (req == 10) {
            r[0] = (uint8_t)alt;
            return 1;
        }
        if (value > (hs ? 3 : 2))
            return -1;
        if (value) {
            usb_iso_out_open(hs ? (value == 1 ? MPS2_16 : MPS2_32) : value == 1 ? MPS16 : MPS24,
                             hs ? (value == 1 ? 4 : 2) : 1);
            usb_iso_in_open(hs ? 4 : 3, hs ? 8 : 1);
            fb_send();
        } else {
            usb_iso_out_close();
            usb_iso_in_close();
        }
        alt = value;
        alt_new = value;
        return 0;
    }
    if (hs)
        return (type & 0x1f) == 1 ? uac2_get(req, (uint8_t)(index >> 8), cs, s[2], r) : -1;
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

/* The host's rate: at once for the feedback (the packet already queued is replaced: the host
 * reads the first ones to know the feedback's format), with the format for the output */
static void new_rate(uint32_t f)
{
    rate = f;
    rate_new = f;
    if (alt) {
        alt_new = alt;
        fb.on = 0;
        usb_iso_in_close();
        usb_iso_in_open(usb.high_speed ? 4 : 3, usb.high_speed ? 8 : 1);
        fb_send();
    }
}

static void uac_setup_out(const uint8_t *s, const uint8_t *d, uint32_t n)
{
    uint8_t req = s[1], cs = s[3];
    uint16_t index = s[4] | s[5] << 8;
    if (req != 0x01)                                    /* SET_CUR only (UAC2: CUR, also 1) */
        return;
    if (usb.high_speed && (index >> 8) == CLOCK_ID) {  /* UAC2: the clock's rate */
        uint32_t f = d[0] | d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
        for (int i = 0; n >= 4 && cs == 1 && i < N_RATES2; i++)
            if (f == rates2[i])
                new_rate(f);
        return;
    }
    if (usb.high_speed ? (index >> 8) != 2 || s[2] != 0 :  /* not feature unit 2 */
        (s[0] & 0x1f) != 2 && index != 0x0200)
        return;
    if ((s[0] & 0x1f) == 2 && cs == 1 && n >= 3) {     /* sampling frequency */
        uint32_t f = d[0] | d[1] << 8 | (uint32_t)d[2] << 16;
        if (f == 44100 || f == 48000)
            new_rate(f);
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
    uint32_t corr_ms;           /* millis() of the last single-frame correction */
    uint32_t frame;             /* bytes of a frame in the ring: 4 (16 bits) or 8 (24 bits) */
    int32_t target, window, settle, ahead;  /* in ring bytes */
    volatile uint32_t last_ms;  /* millis() of the last packet (set by the interrupt) */
    uint32_t packets, dropped, repeated, realigned, bad;
    uint32_t per_ms, sub;       /* packets a millisecond (high speed: 2 or 4); the count */
    int32_t fill_min, fill_max;
} st;
static uint32_t rate_rd0, rate_t0;                      /* the output's real rate, from the DMA */
static uint32_t pkt[(MPS2_32 + 3) / 4], out[2 * 97 + 2];

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
    /* to the ring's format: 16 bits as they are (left low, right high), 24 or 32 bits in the
     * high bits of a 32-bit word per sample (UAC2: as they come, 4-byte subslots) */
    uint32_t frames, k = 0;
    if (alt == 1) {
        frames = n / 4;
        for (uint32_t i = 0; i < frames; i++)
            out[k++] = pkt[i];
    } else if (usb.high_speed) {
        frames = n / 8;
        for (uint32_t i = 0; i < 2 * frames; i++)
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
#ifdef ECHO_UACDIAG
    diag_packet(out, frames, alt == 1);
#endif
    audio_update();
    int32_t fill = audio_fill();
    if (!st.flowing || fill < st.target / 4 || fill > 4 * st.target) {
        if (st.flowing)
            st.realigned++;
        audio_stream_align((uint32_t)st.target);
        if (!st.flowing)
            fb.on = 0;                                  /* the control from scratch */
        st.flowing = 1;
        st.corr = 0;
        fill = audio_fill();
    }
    if (++st.sub >= st.per_ms) {                       /* the control: once a millisecond */
        st.sub = 0;
        fb_control(fill);
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
    int one = st.corr;                                  /* a host that ignores the feedback: */
    if (!one && fb.ignored && (fb.err < -1 || fb.err > 1) &&
        millis() - st.corr_ms >= SPREAD_MS) {           /* one frame, SPREAD_MS apart */
        one = fb.err < 0 ? -1 : 1;
        st.corr_ms = millis();
    }
    uint32_t wpf = st.frame / 4;                        /* words of a frame */
    if (one < 0 && frames > 1) {                        /* the DACs are slower: one less */
        k -= wpf;
        st.dropped++;
    } else if (one > 0) {                               /* faster: the last frame twice */
        for (uint32_t i = 0; i < wpf; i++, k++)
            out[k] = out[k - wpf];
        st.repeated++;
    }
    audio_write(out, 4 * k);
    audio_zero_ahead((uint32_t)st.ahead);
}

/* What to ask of the host: the nominal, plus the control's terms; within 1 % of the nominal */
static uint32_t fb_value(void)
{
    uint32_t nom = fb_nominal(rate), v = nom;
    if (fb.on && !fb.ignored)
        v = (uint32_t)((int32_t)nom + fb.err * FB_KP + fb.integ / 256);
#ifdef ECHO_UACDIAG
    if (diag.step)                              /* 2: -0.5 %, 3: +0.05 %, the others +0.5 % */
        return diag.step == 2 ? nom - nom / 200 : diag.step == 3 ? nom + nom / 2000
                                                                 : nom + nom / 200;
#endif
    if (v > nom + nom / 100)
        v = nom + nom / 100;
    if (v < nom - nom / 100)
        v = nom - nom / 100;
    return v;
}

static void uac_iso_in_done(void)
{
    fb_send();
}

/* The control, at each packet: the fill (frames x 16) averaged, the error, the integral */
static void fb_control(int32_t fill)
{
    int32_t f = fill / (int32_t)st.frame * 16, nom = (int32_t)fb_nominal(rate);
    if (!fb.on) {
        fb.on = 1;
        fb.avg = f;
        fb.integ = 0;
        fb.at_limit = 0;
        fb.ignored = 0;
    }
    fb.avg += (f - fb.avg) / 64;
    fb.err = (st.target / (int32_t)st.frame * 16 - fb.avg) / 16;
    fb.integ += fb.err;
    if (fb.integ > nom / 100 * 256)
        fb.integ = nom / 100 * 256;
    if (fb.integ < -(nom / 100 * 256))
        fb.integ = -(nom / 100 * 256);
    if (fb.integ == nom / 100 * 256 || fb.integ == -(nom / 100 * 256)) {
        if (++fb.at_limit >= IGNORED_MS)
            fb.ignored = 1;
    } else
        fb.at_limit = 0;
}

static void uac_reset(void)
{
    alt = 0;
    alt_new = 0;
}

#ifdef ECHO_UACDIAG
static int uac_setup_diag(const uint8_t *s, uint8_t *r)
{
    int n = uac_setup(s, r);
    diag_record(s, n > 0 ? r : 0, n > 0 ? (uint32_t)n : 0, n < 0);
    return n;
}

static void uac_setup_out_diag(const uint8_t *s, const uint8_t *d, uint32_t n)
{
    diag_record(s, d, n, 0);
    uac_setup_out(s, d, n);
}
#define uac_setup uac_setup_diag
#define uac_setup_out uac_setup_out_diag
#endif

static const struct usb_class uac = {
    .dev = dev_desc, .cfg = cfg_desc, .cfg_len = sizeof cfg_desc,
    .dev_hs = dev_hs, .cfg_hs = cfg_hs, .cfg_hs_len = sizeof cfg_hs,
    .keep_audio_pins = 1,                               /* toggling them while playing clicks */
    .rx_fifo_words = 256, .ptx_fifo_words = 16, .setup = uac_setup, .setup_out = uac_setup_out,
    .iso_out = uac_iso_out, .iso_in_done = uac_iso_in_done, .reset = uac_reset,
    .report = hid_report, .report_len = sizeof hid_report,
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
        audio_start(rate, bits16))              /* muted meanwhile; the volume then ramps up */
        log_printf("uac: output not started");
    cur_rate = rate;
    cur_alt = a;
    st.frame = bits16 ? 4 : 8;
    st.per_ms = usb.high_speed ? (a == 1 ? 2 : 4) : 1;
    st.sub = 0;
    st.target = ms_bytes(TARGET_MS * 1000);
    st.window = ms_bytes(WINDOW_US);
    st.settle = ms_bytes(SETTLE_US);
    st.ahead = ms_bytes(AHEAD_MS * 1000);
    st.fill_min = 0x7fffffff;
    st.fill_max = 0;
    audio_stream_align((uint32_t)st.target);
    fb.on = 0;                                          /* the nominal until packets come */
    rate_rd0 = audio.rd;
    rate_t0 = ticks();
#ifdef ECHO_UACDIAG
    diag_log(0);                                        /* a new period */
#endif
    vol_changed = 1;
    st.ready = 1;
    log_printf("uac: stream %u Hz, %u bits, %s speed", rate, a == 1 ? 16 : a == 2 ? 24 : 32,
               usb.high_speed ? "high" : "full");
}

/* Silence ahead of the DMA when no packet comes (also during a volume ramp) */
static void uac_idle(void)
{
    if (st.flowing)
        return;
    usb_irq_block(1);
    if (!st.flowing)
        audio_idle();
    usb_irq_block(0);
}

/* ---- the screen ---- */
static int level;                               /* our own attenuation, 0.5 dB steps */

static void screen(int line, uint32_t max_gap)
{
    char b[64], *p;
    switch (line) {
    case 0:
    p = put_str(b, !usb.configured ? T("waiting for the host") : !alt ? T("connected, no stream") :
                   st.flowing ? T("playing") : T("stream open, no sound"));
    *p = 0;
    sys_status_line(0, 36, b, usb.configured ? FG : DIM);
    break;
    case 1:
    p = b;
    if (alt) {
        p = put_dec(p, rate / 1000);
        if (rate % 1000) {
            *p++ = '.';
            *p++ = (char)('0' + rate % 1000 / 100);
        }
        p = put_str(p, EN(" kHz, "));
        p = put_dec(p, alt == 1 ? 16 : alt == 2 ? 24 : 32);
        p = put_str(p, EN(" bits, feedback "));
        uint32_t d = (uint32_t)((uint64_t)fb.sent * 10000 >> 14);
        p = put_dec(p, d / 10);
        *p++ = '.';
        p = put_dec(p, d % 10);
    }
    *p = 0;
    sys_status_line(1, 52, b, FG);
    break;
    case 2:
    /* TRANSLATORS: our attenuation (".5" or nothing after its whole dB), the computer's */
    p = log_format(b, b + sizeof b - 1, T("volume -%u%s dB, host -%u dB"), (uint32_t)level / 2,
                   level & 1 ? ".5" : "", (uint32_t)(-host_vol) / 256);
    if (host_mute)
        p = log_format(p, b + sizeof b - 1, ", %s", T("muted"));
    *p = 0;
    sys_status_line(2, 68, b, FG);
    break;
    case 3:
    p = put_str(b, EN("fill "));
    p = put_dec(p, st.frame && rate ? (uint32_t)(audio_fill() > 0 ? audio_fill() : 0) * 1000 /
                                          (rate * st.frame) : 0);
    p = put_str(p, EN(" ms, dropped "));
    p = put_dec(p, st.dropped);
    p = put_str(p, EN(", repeated "));
    p = put_dec(p, st.repeated);
    *p = 0;
    sys_status_line(3, 84, b, DIM);
    break;
    default:
    p = put_str(b, EN("realigned "));
    p = put_dec(p, st.realigned);
    p = put_str(p, EN(", missed "));
    p = put_dec(p, usb.iso_incomplete);
    p = put_str(p, EN(", gap "));
    p = put_dec(p, max_gap / 1000);
    p = put_str(p, EN(" ms"));
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
    usb_irq_block(1);
    audio_update();
    usb_irq_block(0);
    uint32_t dt = ticks() - rate_t0;
    if (rate_t0 && dt > 1000000 && st.frame)
        log_printf("uac: output rate %u.%u Hz over %u ms (nominal %u)",
                   (uint32_t)((uint64_t)(audio.rd - rate_rd0) * 10000000 / st.frame / dt) / 10,
                   (uint32_t)((uint64_t)(audio.rd - rate_rd0) * 10000000 / st.frame / dt) % 10,
                   dt / 1000, rate);
#ifdef ECHO_UACDIAG
    diag_log(dt);
#endif
    rate_rd0 = audio.rd;
    rate_t0 = ticks();
    log_printf("uac: feedback %u (%u.%u Hz), error %d frames, integral %d; IN sent %u, "
               "re-aimed %u%s", fb.sent, (uint32_t)((uint64_t)fb.sent * 10000 >> 14) / 10,
               (uint32_t)((uint64_t)fb.sent * 10000 >> 14) % 10, fb.err, fb.integ / 256,
               usb.iso_in_sent, usb.iso_in_late,
               fb.ignored ? "; the host ignores it: corrections spread" : "");
    log_printf("uac: interrupts %u, the longest %u us", usb.irqs, usb.irq_max_us);
    usb.irq_max_us = 0;
    log_printf("uac: longest parts, us: usb %u, format %u, volume %u, keys %u, screen line %u, "
               "log %u, idle %u", t_max[T_USB], t_max[T_FORMAT], t_max[T_VOLUME], t_max[T_KEYS],
               t_max[T_SCREEN], t_max[T_LOG], t_max[T_IDLE]);
    for (int i = 0; i < T_N; i++)
        t_max[i] = 0;
}

#ifdef ECHO_UACDIAG
/* The feedback step, from the time the stream flows: the figures logged at each change */
static void diag_steps(uint32_t max_gap)
{
    if (!st.flowing) {
        diag.step = 0;
        diag.t0 = 0;
        return;
    }
    if (!diag.t0)
        diag.t0 = millis();
    uint32_t t = millis() - diag.t0;
    int want = t < 20000 || t >= 120000 ? 0 : 1 + (int)((t - 20000) / 20000);
    if (want == diag.step)
        return;
    log_stats("before a feedback step", max_gap);
    usb_irq_block(1);
    diag.step = want;
    if (!want)
        fb.on = 0;                                      /* the control from scratch */
    usb_irq_block(0);
    static const char *const what[] = {
        "off", "+0.5 % in 16.16 per microframe", "-0.5 % in 16.16 per microframe",
        "+0.05 % in 16.16 per microframe", "+0.5 % in 16.16 per ms", "+0.5 % in 10.14 per ms",
    };
    log_printf("uac: feedback step %d: %s", want, what[want]);
}
#endif

/* ---- the headset remote (remote.h): a key's report, then the empty one (the key released),
 * one at a time on endpoint 2; MENU turns it on and off (kept in the settings) ---- */
static struct {
    int open, down;             /* endpoint 2 opened; a key's report sent, the release owed */
    unsigned queue;             /* RC_* to send */
    int step;                   /* the last volume key's step (undone by a double press) */
    struct rc_state rc;
} hid;

static void remote_line(void)
{
    sys_line(116, cfg.uac_remote ? T("MENU: remote on") : T("MENU: remote off"),
             cfg.uac_remote ? FG : DIM);
}

static void hid_send(unsigned bits)
{
    if (cfg.uac_remote && hid.open)
        hid.queue |= bits;
}

static void hid_poll(void)
{
    if (usb.configured && usb.high_speed && !hid.open) {
        usb_irq_block(1);
        usb_int_in_open(8);
        usb_irq_block(0);
        hid.open = 1;
        hid.down = 0;
    } else if (!usb.configured)
        hid.open = hid.down = 0, hid.queue = 0;
    if (!hid.open || usb.int_in_busy || (!hid.down && !hid.queue))
        return;
    static const char *const names[] = { "play/pause", "next", "previous" };
    static uint8_t rep;
    const char *what = "release";
    rep = 0;
    if (!hid.down) {
        unsigned bit = hid.queue & -hid.queue;
        hid.queue &= ~bit;
        rep = (uint8_t)bit;
        what = names[__builtin_ctz(bit)];
    }
    usb_irq_block(1);
    int r = usb_int_in_send(&rep, 1);
    usb_irq_block(0);
    hid.down = r == 0 && rep;
    if (r)
        hid.queue = 0;                                  /* the endpoint is gone: nothing more */
    log_printf("uac: remote: %s sent (%d), %u reports taken", what, r, usb.int_in_sent);
}

void uac_mode_ovl(void)
{
    sys_status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    sys_line(0, T("USB sound card"), TITLE);
    if (!vbus()) {
        sys_line(36, T("plug the USB cable"), WARN);
        mdelay(2000);
        return;
    }
    remote_line();
    sys_line(138, T("VOL+ / VOL-: volume, twice: track"), DIM);
    sys_line(154, T("PLAY: play/pause, BACK held: exit"), DIM);
    hid = (typeof(hid)){ 0 };
    uint8_t remote_was = cfg.uac_remote;
    level = cfg.uac_vol ? cfg.uac_vol - 1 : play_level;     /* its own, once set */
    rate = 48000;
    alt = 0;
    alt_new = -1;
    rate_new = 0;
    host_vol = 0;
    host_mute = 0;
    st = (typeof(st)){ 0 };
    fb = (typeof(fb)){ 0 };
    st.frame = 4;
    audio_stop();                                       /* from scratch: not parked, not paused */
    audio_ring_direct(0);                               /* the SYSRAM ring (ETUDE §103) */
    if (audio_start(rate, 1)) {
        sys_line(36, T("audio output failed"), WARN);
        mdelay(2000);
        return;
    }
    audio_volume(0xff, 0);
    cur_rate = rate;
    cur_alt = 1;
    log_printf("uac: start");
    usb_start_class(&uac);
    ramp_hook = uac_idle;
    usb_irq(1);
    uint32_t t0 = millis(), t_draw = 0, t_key = 0, t_log = millis(), last = ticks(), max_gap = 0;
    uint32_t gap_all = 0, configured_at = 0;
    const char *why = "?";
    int power = 0;
    for (;;) {
        wdt_kick();
        uint32_t now = ticks();
        if (now - last > max_gap)
            max_gap = now - last;
        last = now;
        hid_poll();
        if (usb.configured && !configured_at) {
            configured_at = millis();
            log_printf("uac: configured after %u ms, rx fifo %u words", configured_at - t0,
                       usb.rx_fifo);
        }
        if (alt_new >= 0) {
            uint32_t t = ticks();
            usb_irq_block(1);                           /* the output and the ring change */
            apply_format();
            usb_irq_block(0);
            timed(T_FORMAT, t);
        }
        if (alt && usb.high_speed) {
            usb_irq_block(1);
            usb_iso_watch();
            usb_irq_block(0);
        }
        /* read before millis(): a packet coming in between would put it ahead of millis() */
        uint32_t last_ms = st.last_ms;
        if (st.ready && st.flowing && millis() - last_ms > STALL_MS) {
            st.flowing = 0;                             /* the host stopped sending */
            log_stats("paused", max_gap);
        }
        static int noted;
        if (fb.ignored != noted) {
            noted = fb.ignored;
            if (noted)
                log_printf("uac: the host ignores the feedback (%u s at its limit): back to the "
                           "nominal, corrections spread", IGNORED_MS / 1000);
        }
#ifdef ECHO_UACDIAG
        diag_drain();
        diag_steps(max_gap);
#endif
        if (!st.flowing) {
            uint32_t t = ticks();
            uac_idle();
            timed(T_IDLE, t);
        }
        if (vol_changed) {
            uint32_t t = ticks();
            vol_changed = 0;
            int a = level + (-host_vol) / 128;
            audio_volume(host_mute ? 0xff : (uint8_t)(a > 0xfe ? 0xfe : a), 1);
            timed(T_VOLUME, t);
        }
        if (now - t_key > 10000) {
            t_key = now;
            if (!vbus()) {
                why = "cable unplugged";
                break;
            }
            if (sys_power_poll(ticks(), 1)) {           /* held 2 s: off once the card stops */
                why = "Power 2 s";
                power = 1;
                break;
            }
            struct key_ev e;
            uint32_t tk = ticks();
            int ev = key_poll(&e);
            timed(T_KEYS, tk);
            if ((ev == EV_PRESS || ev == EV_REPEAT) && key_is_vol(e.key)) {
                int d = (e.key == KEY_VOLUP) != !!cfg.swap_vol ? -2 : 2;   /* attenuation */
                unsigned bits;
                if (rc_vol(&hid.rc, d < 0 ? 1 : -1, ev == EV_REPEAT, cfg.uac_remote && hid.open,
                           millis(), &bits) == RC_UNDO) {
                    level -= hid.step;                  /* the first press's step undone */
                    hid.step = 0;
                    hid_send(bits);
                } else {
                    int n = level + d < 0 ? 0 : level + d > 0xfe ? 0xfe : level + d;
                    hid.step = n - level;
                    level = n;
                }
                vol_changed = 1;
            } else if (ev == EV_LONG && e.key == KEY_BACK) {
                why = "BACK";
                break;
            } else if (ev == EV_PRESS && e.key == KEY_PLAY) {
                hid_send(RC_PLAY);
            } else if (ev == EV_PRESS && e.key == KEY_MENU) {
                cfg.uac_remote = !cfg.uac_remote;
                if (!cfg.uac_remote)
                    hid.queue = 0;                      /* a release owed still goes */
                remote_line();
                log_printf("uac: remote %s", cfg.uac_remote ? "on" : "off");
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
    usb_irq(0);
    ramp_hook = 0;
    usb_iso_in_close();
    usb_iso_out_close();
    usb_stop();
    audio_volume(0xff, 0);
    audio_stop();
    if (cfg.uac_vol != level + 1 || cfg.uac_remote != remote_was) {
        cfg.uac_vol = (uint8_t)(level + 1);
        resume_save();                                  /* the settings' block with it */
    }
    log_stats(why, gap_all);
    log_printf("uac: exit (%s) after %u s", why, (millis() - t0) / 1000);
    if (power)
        sys_power_key_off();                            /* the charging screen: the cable is in */
}
