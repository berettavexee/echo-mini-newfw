/* Core B runtime for our codec images (ETUDE §19.2): reset, vectors, mailbox doorbell, the
 * main loop driven by the control block, and the ring helpers. Core B sleeps in WFI whenever
 * it cannot progress; core A wakes it through A2B channel 1 of the mailbox (exception 0x1a). */
#include "rt.h"
#include <stdlib.h>
#include <string.h>

#define MBOX        0x40110000u
#define A2B_INTEN   (MBOX + 0x00)
#define A2B_STATUS  (MBOX + 0x04)
#define NVIC_ISER0  0xe000e100u
#define SCB_VTOR    0xe000ed08u
#define SCB_CFSR    0xe000ed28u
#define IRQ_DOORBELL 0x1a

extern uint32_t _sbss, _ebss;
void hp_reset(void);
static void fault(void);
static void doorbell(void);

static volatile int rung;
static void tick(void);
#ifdef ECHO_BPROBE
static void systick(void);
static volatile int probing;                    /* a fault while set: recorded, passed over */
#endif

__attribute__((section(".vectors"), used))
static void (*const vectors[IRQ_DOORBELL + 1])(void) = {
    (void (*)(void))HP_STACK_TOP, hp_reset,
#ifdef ECHO_BPROBE
    [2 ... 14] = fault, [15] = systick, [16 ... IRQ_DOORBELL - 1] = fault,
#else
    [2 ... 14] = fault, [15] = tick, [16 ... IRQ_DOORBELL - 1] = fault,
#endif
    [IRQ_DOORBELL] = doorbell,
};

/* "wfi; bx lr": a function of its own (the emulator stops core B there) */
__attribute__((noinline)) static void wfi(void)
{
    __asm volatile ("wfi");
}

__attribute__((unused)) static void tick(void)   /* SysTick: room may have come in the ring */
{
    rung = 1;
}

static void doorbell(void)
{
    REG(A2B_STATUS) = 1u << 1;
    rung = 1;
}

/* Hard fault and the like: report and stop */
__attribute__((naked)) static void fault(void)
{
    __asm volatile (
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "b hp_fault\n");
}

static __attribute__((used)) void hp_fault(uint32_t *frame)
{
#ifdef ECHO_BPROBE
    if (probing) {                              /* the probe's access: noted, then skipped */
        uint32_t k = ctl->bp.step;
        if (!ctl->bp.faults) {
            ctl->bp.cfsr = REG(SCB_CFSR);
            ctl->bp.bfar = REG(0xe000ed38u);
            ctl->bp.hfsr = REG(0xe000ed2cu);
        }
        ctl->bp.faults |= 1u << k;
        REG(SCB_CFSR) = REG(SCB_CFSR);          /* write 1 to clear */
        REG(0xe000ed2cu) = REG(0xe000ed2cu);
        uint16_t op = *(const uint16_t *)frame[6];
        frame[6] += (op & 0xf800) >= 0xe800 ? 4 : 2;    /* a 32-bit Thumb instruction or not */
        return;
    }
#endif
    ctl->fault_pc = frame[6];
    ctl->fault_lr = frame[5];
    ctl->fault_cfsr = REG(SCB_CFSR);
    ctl->error = HP_ERR_FAULT;
    ctl->state = HP_ERROR;
    for (;;)
        wfi();
}

/* ---- rings ---- */
uint32_t in_avail(void)
{
    return ctl->in_wr - ctl->in_rd;
}

int in_ended(void)
{
    return ctl->in_end != 0xffffffffu && ctl->in_wr == ctl->in_end;
}

static uint32_t peek_at(uint32_t off, void *dst, uint32_t n)
{
    uint32_t a = in_avail(), o = (ctl->in_rd + off) % HP_IN_SIZE;
    a = a > off ? a - off : 0;
    if (n > a)
        n = a;
    uint32_t k = HP_IN_SIZE - o < n ? HP_IN_SIZE - o : n;
    memcpy(dst, (const void *)(HP_IN_BASE + o), k);
    memcpy((uint8_t *)dst + k, (const void *)HP_IN_BASE, n - k);
    return n;
}

uint32_t in_peek(void *dst, uint32_t n)
{
    return peek_at(0, dst, n);
}

uint32_t in_peek_ahead(uint32_t off, void *dst, uint32_t n)
{
    /* core A refills when 8 KiB are free (player.c feed()) */
    while (in_avail() < off + n && !in_ended() && in_avail() < HP_IN_SIZE - 8192)
        rt_wait();
    return peek_at(off, dst, n);
}

uint32_t in_read_wait(void *dst, uint32_t n)
{
    uint32_t got = 0;
    while (got < n) {
        uint32_t k = in_read((uint8_t *)dst + got, n - got);
        got += k;
        if (!k) {
            if (in_ended())
                break;
            rt_wait();
        }
    }
    return got;
}

/* Forward only. Within what core A has written: consumed here; beyond: in_rd is set past in_wr,
 * which tells core A to go on reading from there (its write position follows). */
/* Backwards: core A is asked to feed again from pos (the ring emptied), then waited for */
static int in_rewind(uint32_t pos)
{
    uint32_t seq = ctl->rewind_seq + 1;
    ctl->rewind_pos = pos;
    ctl->rewind_seq = seq;                      /* core A polls it with the ring */
    while (ctl->rewind_ack != seq)
        rt_wait();
    return 0;
}

int in_seek(uint32_t pos)
{
    if ((int32_t)(pos - ctl->in_rd) < 0)
        return in_rewind(pos);
    if (ctl->in_end != 0xffffffffu && pos > ctl->in_end)
        return -1;
    if ((int32_t)(pos - ctl->in_wr) <= 0)
        ctl->in_rd = pos;
    else {
        ctl->in_rd = pos;
        while ((int32_t)(ctl->in_wr - pos) < 0)
            rt_wait();
    }
    return 0;
}

uint32_t in_read(void *dst, uint32_t n)
{
    n = in_peek(dst, n);
    ctl->in_rd += n;
    return n;
}

void in_skip(uint32_t n)
{
    uint32_t a = in_avail();
    ctl->in_rd += n < a ? n : a;
}

/* ---- the PCM ring the DMA reads (version 2, ETUDE §103) ----
 * The DMA's position is read from its SAR at each look; a look later than half a ring (core B
 * busy that long: not expected) would hide laps, counted from TIMER1 as core A did (§97). The
 * DMA overtaking what core B wrote (an underrun: input late, or core B too slow) leaves stale
 * sound played; the write position then jumps to the DMA's and the episode is counted. */
#define ST_CSR 0xe000e010u
#define ST_RVR 0xe000e014u
#define ST_CVR 0xe000e018u

static uint32_t st_hz;                          /* the core clock the SysTick is set for */
static int pcm_started;                         /* a frame written: a late DMA is an underrun
                                                 * (before, the silence ahead of a seek runs out
                                                 * while the first frames decode: device, r21) */

static void pcm_update(void)
{
    uint32_t pos = REG(HP_DMA_SAR) - HP_OUT_BASE, now = TIMER1_NOW();
    if (pos < HP_OUT_SIZE && ctl->pcm_bps) {
        uint32_t moved = (pos + HP_OUT_SIZE - ctl->pcm_pos) % HP_OUT_SIZE;
        uint64_t played = (uint64_t)(now - ctl->pcm_t) * ctl->pcm_bps / 24000000;
        if (played > HP_OUT_SIZE / 2 && played > moved)
            moved += (uint32_t)((played - moved + HP_OUT_SIZE / 2) / HP_OUT_SIZE) * HP_OUT_SIZE;
        ctl->pcm_rd += moved;
        ctl->pcm_pos = pos;
    }
    ctl->pcm_t = now;
    int32_t fill = (int32_t)(ctl->pcm_wr - ctl->pcm_rd);
    if (fill < 0) {
        if (pcm_started) {
            ctl->pcm_ur++;
            ctl->pcm_lost += (uint32_t)-fill;
        }
        ctl->pcm_wr = (ctl->pcm_rd + 7) & ~7u;  /* whole frames, 4 or 8 bytes */
        fill = 0;
    }
    if ((uint32_t)fill < ctl->pcm_min)
        ctl->pcm_min = (uint32_t)fill;
    if (ctl->b_hz != st_hz) {                   /* a wake every 2 ms while there is room */
        st_hz = ctl->b_hz;
        REG(ST_CSR) = 0;
        if (st_hz) {
            uint32_t load = st_hz / 500;
            REG(ST_RVR) = (load > 0xffffff ? 0xffffff : load) - 1;
            REG(ST_CVR) = 0;
            REG(ST_CSR) = 7;                    /* the core clock, its interrupt, on */
        }
    }
}

static uint32_t raw_space(void)
{
    if (ctl->pcm_hold)
        return 0;
    pcm_update();
    uint32_t fill = ctl->pcm_wr - ctl->pcm_rd;
    return fill < HP_OUT_SIZE - HP_OUT_GUARD ? HP_OUT_SIZE - HP_OUT_GUARD - fill : 0;
}

/* ---- battery beeps (0.99r20, the mixer core A had, ETUDE §43): while a pattern runs, the
 * music is lowered by 12 dB and a sine at -12 dBFS (5 ms fades) is added ---- */
static const int16_t beep_sine[64] = {
    0, 803, 1598, 2378, 3135, 3862, 4551, 5197, 5793, 6333, 6811, 7225, 7568, 7839, 8035, 8153,
    8192, 8153, 8035, 7839, 7568, 7225, 6811, 6333, 5793, 5197, 4551, 3862, 3135, 2378, 1598, 803,
    0, -803, -1598, -2378, -3135, -3862, -4551, -5197, -5793, -6333, -6811, -7225, -7568, -7839,
    -8035, -8153, -8192, -8153, -8035, -7839, -7568, -7225, -6811, -6333, -5793, -5197, -4551,
    -3862, -3135, -2378, -1598, -803,
};
static struct { int on, mix; uint32_t seq, frame, phase; int32_t t; } bp;

static void beep_check(void)                    /* a new pattern from core A */
{
    if (ctl->beep_seq != bp.seq) {
        bp.seq = ctl->beep_seq;
        bp.frame = ctl->beep_from;
        bp.phase = 0;
        bp.on = ctl->beep_period && bp.frame < ctl->beep_total;
    }
}

static int32_t beep_sample(void)
{
    uint32_t f = bp.frame % ctl->beep_period, on = ctl->beep_on, fade = ctl->beep_fade;
    int32_t v = 0;
    if (f < on) {
        uint32_t edge = f < on - f ? f : on - f;
        v = beep_sine[bp.phase >> 26];
        if (edge < fade)
            v = v * (int32_t)edge / (int32_t)fade;
        bp.phase += ctl->beep_step;
    }
    if (++bp.frame >= ctl->beep_total)
        bp.on = 0;
    ctl->beep_frame = bp.frame;
    return v;
}

/* One word on its way to the ring at byte counter 'at' */
static uint32_t beep_mix(uint32_t w, uint32_t at)
{
    if (ctl->frame_bytes == 4) {                /* one word: left low, right high */
        if (!bp.on)
            return w;
        int32_t t = beep_sample();
        int32_t l = (int16_t)(w & 0xffff) / 4 + t, r = (int16_t)(w >> 16) / 4 + t;
        return ((uint32_t)l & 0xffff) | (uint32_t)r << 16;
    }
    if (!(at & 4)) {                            /* left word: a new frame */
        bp.mix = bp.on;
        if (bp.mix)
            bp.t = beep_sample();
    }
    return bp.mix ? (uint32_t)(((int32_t)w >> 2) + (bp.t << 16)) : w;
}

static void raw_word(uint32_t w);

static void raw_write(const uint32_t *w, uint32_t words)
{
    beep_check();
    if (bp.on || bp.mix) {                      /* word by word while a beep runs */
        while (words--)
            raw_word(*w++);
        return;
    }
    uint32_t o = ctl->pcm_wr % HP_OUT_SIZE, n = words * 4;
    uint32_t k = HP_OUT_SIZE - o < n ? HP_OUT_SIZE - o : n;
    pcm_started = 1;
    memcpy((void *)(HP_OUT_BASE + o), w, k);
    memcpy((void *)HP_OUT_BASE, (const uint8_t *)w + k, n - k);
    ctl->pcm_wr += n;
}

static void raw_word(uint32_t w)
{
    uint32_t at = ctl->pcm_wr;
    beep_check();
    if (bp.on || bp.mix)
        w = beep_mix(w, at);
    *(volatile uint32_t *)(HP_OUT_BASE + at % HP_OUT_SIZE) = w;
    ctl->pcm_wr = at + 4;
    pcm_started = 1;
}

/* ---- upsampling (ETUDE §59) ----
 * A rate the DACs do not take is raised by the smallest integer factor L (2, 3, 4 or 6) that gives
 * 44.1, 48, 88.2 or 96 kHz. Since 0.79 the DACs take the 15 rates of the stock firmware (8 to
 * 192 kHz, ETUDE §60): only unusual rates (7.35 kHz...) come here.
 * after codec_open, the rate, the length and the start are multiplied by L, out_space() is
 * divided by it, and every frame the codec writes goes through a polyphase windowed-sinc filter
 * (hifi/upsample_tab.h, tools/mkupsample.py: 24 taps per phase, -0.1 dB up to 0.4 of the input
 * rate, -82 dB from 0.6) before the ring. The filter's delay (a whole number of output frames:
 * odd length) is dropped at the start and flushed at the end: the output has exactly L times the
 * frames of the input (gapless kept). ctl->frames counts the frames written to the ring, as they
 * are written (out_done() of the codecs does nothing then). */
#include "upsample_tab.h"

static struct {
    uint32_t L;                         /* 1: off */
    const int16_t *h;
    uint32_t fb;                        /* bytes per frame in the ring (4 or 8) */
    uint32_t half, have;                /* 8-byte frames: the left word, kept until the right */
    uint32_t pos;                       /* next place in hist */
    uint32_t drop;                      /* output frames still to drop: the filter delay */
    uint32_t tail;                      /* at the end: output frames still to write */
    int flushing;
    int32_t hist[2][2 * UP_TAPS];       /* each sample twice: T in a row from any place */
} up;

static int up_setup(void)
{
    static const uint32_t dac[] = { 44100, 48000, 88200, 96000, 176400, 192000, 8000, 11025, 12000,
                                    16000, 22050, 24000, 32000, 64000, 128000 };   /* §60 */
    static const uint8_t factors[] = { 2, 3, 4, 6 };
    uint32_t r = ctl->rate;
    up.L = 1;
    for (unsigned i = 0; i < sizeof dac / sizeof *dac; i++)
        if (r == dac[i])
            return 0;
    for (unsigned f = 0; f < sizeof factors && up.L == 1; f++)
        for (unsigned i = 0; i < 4; i++)        /* up to 96 kHz */
            if (r * factors[f] == dac[i])
                up.L = factors[f];
    if (up.L == 1)
        return -HP_ERR_UNSUPPORTED;
    up.h = up.L == 2 ? up_h2 : up.L == 3 ? up_h3 : up.L == 4 ? up_h4 : up_h6;
    up.fb = ctl->frame_bytes;
    up.have = up.pos = up.flushing = 0;
    up.drop = UP_TAPS * up.L / 2 - 1;       /* (taps - 1) / 2, with taps = T * L - 1 */
    memset(up.hist, 0, sizeof up.hist);
    ctl->rate = r * up.L;
    ctl->total_frames *= up.L;
    ctl->start_frame *= up.L;
    return 0;
}

static inline int32_t sat(int32_t v, int32_t lim)
{
    return v > lim - 1 ? lim - 1 : v < -lim ? -lim : v;
}

/* One input frame (16-bit samples, or 24-bit right-aligned) into L output frames */
static void up_frame(int32_t l, int32_t r)
{
    uint32_t p = up.pos, L = up.L;
    up.hist[0][p] = up.hist[0][p + UP_TAPS] = l;
    up.hist[1][p] = up.hist[1][p + UP_TAPS] = r;
    up.pos = p + 1 == UP_TAPS ? 0 : p + 1;
    const int32_t *xl = &up.hist[0][p + UP_TAPS], *xr = &up.hist[1][p + UP_TAPS];
    for (uint32_t q = 0; q < L; q++) {
        const int16_t *h = up.h + q;
        if (up.fb == 4) {
            int32_t al = 0, ar = 0;
            for (int k = 0; k < UP_TAPS; k++, h += L) {
                al += *h * xl[-k];
                ar += *h * xr[-k];
            }
            if (up.drop) {
                up.drop--;
                continue;
            }
            if (up.flushing && !up.tail--)
                return;
            raw_word((uint16_t)sat(al >> 14, 32768) | (uint32_t)sat(ar >> 14, 32768) << 16);
            ctl->frames++;
        } else {
            int64_t al = 0, ar = 0;
            for (int k = 0; k < UP_TAPS; k++, h += L) {
                al += (int64_t)*h * xl[-k];
                ar += (int64_t)*h * xr[-k];
            }
            if (up.drop) {
                up.drop--;
                continue;
            }
            if (up.flushing && !up.tail--)
                return;
            raw_word((uint32_t)sat((int32_t)(al >> 14), 1 << 23) << 8);
            raw_word((uint32_t)sat((int32_t)(ar >> 14), 1 << 23) << 8);
            ctl->frames++;
        }
    }
}

static void up_word(uint32_t w)
{
    if (up.fb == 4)
        up_frame((int16_t)w, (int16_t)(w >> 16));
    else if (!up.have) {
        up.half = w;
        up.have = 1;
    } else {
        up.have = 0;
        up_frame((int32_t)up.half >> 8, (int32_t)w >> 8);
    }
}

/* At the end of the stream: silence pushes the last samples out, exactly the frames dropped at
 * the start */
static void up_flush(void)
{
    up.flushing = 1;
    up.tail = UP_TAPS * up.L / 2 - 1;
    for (uint32_t i = 0; i < UP_TAPS / 2 && up.tail; i++) {
        while (raw_space() < up.L * up.fb)
            rt_wait();
        up_frame(0, 0);
    }
}

uint32_t out_space(void)
{
    uint32_t s = raw_space();
    if (up.L <= 1)
        return s;
    s /= up.L;
    return s - s % up.fb;                       /* whole input frames */
}

void out_write(const uint32_t *w, uint32_t words)
{
    if (up.L <= 1) {
        raw_write(w, words);
        return;
    }
    while (words--)
        up_word(*w++);
}

void out_done(uint32_t frames)
{
    if (up.L <= 1)                              /* upsampled: counted in up_frame() */
        ctl->frames += frames;
}

void out_word(uint32_t w)
{
    if (up.L <= 1)
        raw_word(w);
    else
        up_word(w);
}

/* ---- heap ---- */
static uint32_t heap_base, heap_next;
uint32_t rt_heap_peak;

void rt_heap_reset(void)
{
    heap_next = heap_base = ((uint32_t)&_ebss + 7) & ~7u;
}

void *rt_alloc(uint32_t n)
{
    if (!heap_next)
        rt_heap_reset();
    n = (n + 7) & ~7u;
    if (n > HP_DATA_END - heap_next)
        return 0;
    void *p = (void *)heap_next;
    heap_next += n;
    if (heap_next - heap_base > rt_heap_peak)
        rt_heap_peak = heap_next - heap_base;
    return p;
}

/* ---- the C library functions the compiler and the decoders call ---- */
void *memcpy(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (!(((uint32_t)dp | (uint32_t)sp) & 3)) {
        for (; n >= 16; n -= 16, dp += 16, sp += 16) {
            ((uint32_t *)dp)[0] = ((const uint32_t *)sp)[0];
            ((uint32_t *)dp)[1] = ((const uint32_t *)sp)[1];
            ((uint32_t *)dp)[2] = ((const uint32_t *)sp)[2];
            ((uint32_t *)dp)[3] = ((const uint32_t *)sp)[3];
        }
        for (; n >= 4; n -= 4, dp += 4, sp += 4)
            *(uint32_t *)dp = *(const uint32_t *)sp;
    }
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp <= sp || dp >= sp + n)
        return memcpy(d, s, n);
    while (n--)
        dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, size_t n)
{
    uint8_t *dp = d;
    while (n--)
        *dp++ = (uint8_t)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

/* ---- main loop ---- */
static void sleep_until_rung(void)
{
    __asm volatile ("cpsid i");
    if (!rung) {
        uint32_t t0 = TIMER1_NOW();
        ctl->sleep_since = t0 | 1;
        wfi();                                  /* a pending doorbell wakes it at once */
        ctl->idle_ticks += TIMER1_NOW() - t0;
        ctl->sleep_since = 0;
    }
    rung = 0;
    __asm volatile ("cpsie i");
    ctl->wakeups++;
}

void rt_wait(void)
{
    sleep_until_rung();
}

static void error(int e)
{
    ctl->error = (uint32_t)-e;
    ctl->state = HP_ERROR;
}

static void run(void)
{
    for (;;) {
        uint32_t seq = ctl->cmd_seq;
        if (seq != ctl->ack_seq) {
            uint32_t cmd = ctl->cmd;
            ctl->ack_seq = seq;
            if (cmd == HP_CMD_OPEN && ctl->state == HP_READY)
                ctl->state = HP_OPENING;
            else if (cmd == HP_CMD_PLAY && ctl->state == HP_FORMAT)
                ctl->state = HP_PLAYING;
            else if (cmd == HP_CMD_STOP)
                ctl->state = HP_READY;
        }
        int r = 0;
        ctl->work_loops++;
        if (ctl->state == HP_OPENING) {
            r = codec_open();
            if (r == 0)
                r = up_setup();                 /* a rate the DACs do not take: upsampled */
            if (r == 0) {
                ctl->state = HP_FORMAT;
                r = 1;
            } else if (r > 0)
                r = 0;
            else
                error(r);
        } else if (ctl->state == HP_PLAYING) {
            r = codec_run();
            if (r == 2 && up.L > 1)
                up_flush();
            if (r == 2)
                ctl->state = HP_END;
            else if (r < 0)
                error(r);
        }
        if (r != 1)
            sleep_until_rung();
    }
}


#ifdef ECHO_BPROBE
/* ---- the probe of the test build (0.99r17): see hp_ctl.bp ---- */
#define ST_CSR   0xe000e010u
#define ST_RVR   0xe000e014u
#define ST_CVR   0xe000e018u
#define ST_CALIB 0xe000e01cu
#define ACTLR    0xe000e008u
static volatile uint32_t st_count;

static void systick(void)
{
    st_count++;
}

static void spin_ticks(uint32_t t)              /* TIMER1: 24 MHz */
{
    uint32_t t0 = TIMER1_NOW();
    while (TIMER1_NOW() - t0 < t)
        ;
}

static uint32_t st_measure(uint32_t csr)       /* SysTick counts in 1 ms (no interrupt) */
{
    REG(ST_CSR) = 0;
    REG(ST_RVR) = 0xffffff;
    REG(ST_CVR) = 0;
    REG(ST_CSR) = csr;
    uint32_t a = REG(ST_CVR);
    spin_ticks(24000);
    uint32_t b = REG(ST_CVR);
    REG(ST_CSR) = 0;
    return (a - b) & 0xffffff;
}

static void probe(void)
{
    volatile uint32_t *got = ctl->bp.got;
    REG(ACTLR) |= 2;                            /* DISDEFWBUF: a write's fault is precise */
    probing = 1;
    ctl->bp.step = BP_SYS_RD;
    got[BP_SYS_RD] = REG(ctl->bp.sys_addr);
    ctl->bp.step = BP_SYS_WR;
    REG(ctl->bp.sys_addr + 4) = ~ctl->bp.sys_word;
    got[BP_SYS_WR] = REG(ctl->bp.sys_addr + 4);
    ctl->bp.step = BP_DRAM_RD;
    got[BP_DRAM_RD] = REG(ctl->bp.dram_addr);
    ctl->bp.step = BP_PMU_RD;
    got[BP_PMU_RD] = REG(ctl->bp.pmu_addr);
    ctl->bp.step = BP_PMU_WR;
    REG(ctl->bp.pmu_addr + 4) = ~ctl->bp.pmu_word;
    got[BP_PMU_WR] = REG(ctl->bp.pmu_addr + 4);
    ctl->bp.step = BP_SAR;
    uint32_t t0 = TIMER1_NOW();
    ctl->bp.sar0 = REG(0x60000000u + 5 * 0x58);
    spin_ticks(24000);
    ctl->bp.sar1 = REG(0x60000000u + 5 * 0x58);
    ctl->bp.sar_dt = TIMER1_NOW() - t0;
    ctl->bp.step = BP_SYSTICK;
    ctl->bp.st_calib = REG(ST_CALIB);
    ctl->bp.st_core = st_measure(5);            /* CLKSOURCE 1: the core clock */
    ctl->bp.st_ref = st_measure(1);             /* CLKSOURCE 0: the reference clock */
    uint32_t load = ctl->bp.st_core * 2;        /* 2 ms on the core clock */
    if (load > 0xffffff)
        load = 0xffffff;
    ctl->bp.st_load = load;
    if (load > 1000) {
        st_count = 0;
        REG(ST_RVR) = load - 1;
        REG(ST_CVR) = 0;
        REG(ST_CSR) = 7;                        /* core clock, interrupt, enabled */
        spin_ticks(20 * 24000);
        REG(ST_CSR) = 0;
        ctl->bp.st_irqs = st_count;
    }
    probing = 0;
    REG(ACTLR) &= ~2u;
    ctl->bp.step = BP_DONE;
}
#endif

__attribute__((noreturn)) void hp_reset(void)
{
    for (uint32_t *p = &_sbss; p < &_ebss; p++)
        *p = 0;
    REG(SCB_VTOR) = 0;
    ctl->codec = codec_id;
    ctl->version = HP_VERSION;
    ctl->error = HP_ERR_NONE;
    ctl->out_wr = ctl->frames = ctl->total_frames = ctl->start_frame = 0;
    ctl->in_rd = 0;
    ctl->ack_seq = ctl->cmd_seq;
    ctl->magic = HP_MAGIC;
    REG(A2B_STATUS) = 0xf;
    REG(A2B_INTEN) |= 1u << 1;
    REG(NVIC_ISER0) = 1u << (IRQ_DOORBELL - 16);
#ifdef ECHO_BPROBE
    if (ctl->bp.req == BP_REQ)
        probe();
#endif
    ctl->state = HP_READY;
    run();
    for (;;)
        ;
}
