/* Core B runtime for our codec images (ETUDE §19.2): reset, vectors, mailbox doorbell, the
 * main loop driven by the control block, and the ring helpers. Core B sleeps in WFI whenever
 * it cannot progress; core A wakes it through A2B channel 1 of the mailbox (exception 0x1a). */
#include "rt.h"
#include <string.h>

#define MBOX        0x40110000u
#define A2B_INTEN   (MBOX + 0x00)
#define A2B_STATUS  (MBOX + 0x04)
#define NVIC_ISER0  0xe000e100u
#define SCB_VTOR    0xe000ed08u
#define SCB_CFSR    0xe000ed28u
#define REG(a)      (*(volatile uint32_t *)(a))
#define IRQ_DOORBELL 0x1a

extern uint32_t _sbss, _ebss;
void hp_reset(void);
static void fault(void);
static void doorbell(void);

static volatile int rung;

__attribute__((section(".vectors"), used))
static void (*const vectors[IRQ_DOORBELL + 1])(void) = {
    (void (*)(void))HP_STACK_TOP, hp_reset,
    [2 ... IRQ_DOORBELL - 1] = fault,
    [IRQ_DOORBELL] = doorbell,
};

/* "wfi; bx lr": a function of its own (the emulator stops core B there) */
__attribute__((noinline)) static void wfi(void)
{
    __asm volatile ("wfi");
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

__attribute__((used)) void hp_fault(const uint32_t *frame)
{
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
int in_seek(uint32_t pos)
{
    if ((int32_t)(pos - ctl->in_rd) < 0)
        return -1;
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

uint32_t out_space(void)
{
    uint32_t fill = (ctl->out_wr + HP_OUT_SIZE - ctl->out_rd) % HP_OUT_SIZE;
    return fill < HP_OUT_SIZE - HP_OUT_GUARD ? HP_OUT_SIZE - HP_OUT_GUARD - fill : 0;
}

void out_write(const uint32_t *w, uint32_t words)
{
    uint32_t o = ctl->out_wr, n = words * 4;
    uint32_t k = HP_OUT_SIZE - o < n ? HP_OUT_SIZE - o : n;
    memcpy((void *)(HP_OUT_BASE + o), w, k);
    memcpy((void *)HP_OUT_BASE, (const uint8_t *)w + k, n - k);
    o += n;
    ctl->out_wr = o >= HP_OUT_SIZE ? o - HP_OUT_SIZE : o;
}

void out_word(uint32_t w)
{
    uint32_t o = ctl->out_wr;
    *(volatile uint32_t *)(HP_OUT_BASE + o) = w;
    o += 4;
    ctl->out_wr = o == HP_OUT_SIZE ? 0 : o;
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
#define TIMER1_CUR0 0x400e0028u               /* 24 MHz, shared with core A */

static void sleep_until_rung(void)
{
    __asm volatile ("cpsid i");
    if (!rung) {
        uint32_t t0 = ~REG(TIMER1_CUR0);
        ctl->sleep_since = t0 | 1;
        wfi();                                  /* a pending doorbell wakes it at once */
        ctl->idle_ticks += ~REG(TIMER1_CUR0) - t0;
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
            if (r == 0) {
                ctl->state = HP_FORMAT;
                r = 1;
            } else if (r > 0)
                r = 0;
            else
                error(r);
        } else if (ctl->state == HP_PLAYING) {
            r = codec_run();
            if (r == 2)
                ctl->state = HP_END;
            else if (r < 0)
                error(r);
        }
        if (r != 1)
            sleep_until_rung();
    }
}

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
    ctl->state = HP_READY;
    run();
    for (;;)
        ;
}
