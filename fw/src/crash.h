/* The fault record (rework step 7, 0.99r32, ETUDE §109): one sector of NEWFWLOG.TXT, before the
 * resume state, written as text by the fault handler itself (the eMMC driver polls: no
 * interrupt needed), then the log ring flushed into the file: a fault survives a power off,
 * which the PMU-SRAM ring does not. The next boot logs it ("crash:"), shows it once on the
 * screen and marks it read. tools/readlog.py prints it apart from the log. */
#ifndef CRASH_H
#define CRASH_H

#include <stdint.h>

#define CRASH_MAGIC "NEWFWCRASH 1"

enum { CRASH_A = 'A', CRASH_B = 'B' };

struct crash_info {
    char core;                  /* CRASH_A, CRASH_B */
    const char *what;           /* "HardFault", "core B fault", "core B stall", "watchdog" */
    uint32_t exc, pc, lr, psr, sp, cfsr, hfsr, bfar, mmfar;
    const uint32_t *stack;      /* 16 words above the exception frame, or 0 */
};

/* Writes the record and flushes the log; safe from a fault handler (nothing is written when the
 * fault came from inside the eMMC driver, or when the log is not attached: USB mode) */
void crash_record(const struct crash_info *c);
/* At boot, after log_attach(): the record read, logged if unread, then marked read */
void crash_boot(void);

extern int crash_due;           /* an unread record found at boot: the screen shows it once */
extern char crash_text[2][48];  /* its two lines for the screen */

#endif
