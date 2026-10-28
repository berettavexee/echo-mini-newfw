/* USB probe (test build only, make EXTRA=-DECHO_PROBE, ETUDE §72): a mailbox in the file
 * PROBE.BIN of the internal volume, served by the mass storage driver while in USB mode */
#ifndef PROBE_H
#define PROBE_H
#include <stdint.h>

#define PROBE_SECTORS 8                 /* a request and its answer: 4 KiB */
#define PROBE_REQ     0x31425250u       /* "PRB1" */
#define PROBE_ANS     0x52425250u       /* "PRBR" */
enum { PROBE_INFO = 1, PROBE_PEEK, PROBE_POKE, PROBE_POWER_B, PROBE_RUN_B, PROBE_STOP_B,
       PROBE_INSTALL };
enum { PROBE_OK, PROBE_EMAGIC, PROBE_ECMD, PROBE_EADDR, PROBE_ELEN };

struct probe_msg {                      /* request (magic PROBE_REQ) and answer (PROBE_ANS) */
    uint32_t magic, seq, cmd, status;   /* status: in the answer */
    uint32_t addr, n, arg, count;       /* count: requests served since USB mode started */
    uint32_t data[(PROBE_SECTORS * 512 - 32) / 4];
};

/* At the start of USB mode: finds PROBE.BIN (msc LBA = volume sector - base); 0 if absent */
int probe_attach(uint32_t base);
int probe_hit(uint32_t lba, uint32_t n);        /* does [lba, lba + n) touch the mailbox */
void probe_write(uint32_t lba, uint32_t n, const void *data);
void probe_read(uint32_t lba, uint32_t n, void *data);
void probe_after_usb(void);                     /* a pending install: done, then reboot */
#endif
