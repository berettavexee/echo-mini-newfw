/* The fault record: see crash.h. A text sector, so that the file can be read with a text editor
 * as well as with tools/readlog.py:
 *
 *   NEWFWCRASH 1 unread=1 seq=00000003
 *   version 0.99r32 (2026-11-01), boot 5, at 123.456 s
 *   core A: HardFault, exception 3, task player
 *   pc 03051234 lr 03052345 psr 21000000 sp 03003f20
 *   cfsr 00008200 hfsr 40000000 bfar fffffff0 mmfar fffffff0
 *   stack 00000000 ... (16 words)
 *   track 01ALSO~1.OPU at 12345 ms
 *
 * then spaces to the end of the sector, its last byte a newline. */
#include "crash.h"
#include "board.h"
#include "emmc.h"
#include "log.h"
#include "perf.h"
#include "player.h"
#include "version.h"

#define UNREAD_AT 20                            /* the '0' or '1' after "unread=" */

int crash_due;
char crash_text[2][48];
static uint32_t seq;                            /* the last record's number, read at boot */
static uint8_t sec[512];

static int is_record(const uint8_t *b)
{
    for (int i = 0; i < 12; i++)
        if (b[i] != (uint8_t)CRASH_MAGIC[i])
            return 0;
    return 1;
}

static uint32_t hex8(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = p[i];
        v = v << 4 | (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

void crash_record(const struct crash_info *c)
{
    if (!logs.attached || emmc_busy)            /* USB mode, or the fault is the driver's */
        return;
    char *p = (char *)sec, *end = p + 511;
    uint32_t ms = millis();
    const char *task = perf_phase(), *name = player_file_name();
    p = log_format(p, end, CRASH_MAGIC " unread=1 seq=%08x\n", ++seq);
    p = log_format(p, end, "version " FW_VERSION " (" FW_DATE "), boot %u, at %u.%03u s\n",
                   logs.boots, ms / 1000, ms % 1000);
    p = log_format(p, end, "core %c: %s, exception %u, task %s\n", c->core, c->what, c->exc,
                   task ? task : "-");
    p = log_format(p, end, "pc %08x lr %08x psr %08x sp %08x\n", c->pc, c->lr, c->psr, c->sp);
    p = log_format(p, end, "cfsr %08x hfsr %08x", c->cfsr, c->hfsr);
    if (c->cfsr & 0x8000)                       /* BFARVALID */
        p = log_format(p, end, " bfar %08x", c->bfar);
    if (c->cfsr & 0x80)                         /* MMARVALID */
        p = log_format(p, end, " mmfar %08x", c->mmfar);
    if (c->cfsr & 0x400)                        /* IMPRECISERR (0.99r35) */
        p = log_format(p, end, " (imprecise: the pc is after the faulting write)");
    p = log_format(p, end, "\n");
    if (c->stack) {
        p = log_format(p, end, "stack");
        for (int i = 0; i < 16; i++)
            p = log_format(p, end, " %08x", c->stack[i]);
        p = log_format(p, end, "\n");
    }
    if (name && name[0])
        p = log_format(p, end, "track %s at %u ms\n", name, player.time_ms);
    while (p < end)
        *p++ = ' ';
    sec[511] = '\n';
    log_crash_write(sec);
    log_flush();                                /* the lines before the fault, into the file */
}

void crash_boot(void)
{
    crash_due = 0;
    if (log_crash_read(sec))
        return;
    if (!is_record(sec)) {                      /* a file from before 0.99r32: the sector made */
        char *p = (char *)sec, *end = p + 511;  /* a record (none yet) */
        p = log_format(p, end, CRASH_MAGIC " unread=0 seq=00000000\nno fault recorded\n");
        while (p < end)
            *p++ = ' ';
        sec[511] = '\n';
        seq = 0;
        log_crash_write(sec);
        return;
    }
    seq = hex8(sec + 26);
    if (sec[UNREAD_AT] != '1')
        return;
    /* its lines after the first (version, core, registers, track) into the log; the core and
     * the pc lines kept for the screen */
    log_printf("crash: the last session ended on a fault (record %u):", seq);
    int line = 0, k = 0;
    char b[96];
    for (int i = 0; i < 511; i++) {
        if (sec[i] != '\n') {
            if (line && k < (int)sizeof b - 1)
                b[k++] = (char)sec[i];
            continue;
        }
        b[k] = 0;
        if (line && k)
            log_printf("crash: %s", b);
        if (line == 2 || line == 3)
            for (int j = 0; j < 48; j++)
                crash_text[line - 2][j] = j < 47 && j <= k ? b[j] : 0;
        line++;
        k = 0;
    }
    sec[UNREAD_AT] = '0';
    log_crash_write(sec);
    crash_due = 1;
}
