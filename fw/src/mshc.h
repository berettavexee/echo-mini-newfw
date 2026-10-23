/* DesignWare SD/MMC host controllers (TRM chapters 10-11), data moved by the CPU through the
 * 16-word FIFO: the eMMC at 0x60030000, the microSD card at 0x60020000 (ETUDE §3.4, §36) */
#ifndef MSHC_H
#define MSHC_H

#include <stdint.h>
#include "emmc.h"               /* error codes EMMC_E* */

#define MSHC_EMMC 0x60030000u
#define MSHC_SD   0x60020000u

#define MSHC_CTRL    0x00
#define MSHC_PWREN   0x04
#define MSHC_CLKDIV  0x08
#define MSHC_CLKENA  0x10
#define MSHC_TMOUT   0x14
#define MSHC_CTYPE   0x18
#define MSHC_INTMASK 0x24
#define MSHC_RESP(i) (0x30 + 4 * (i))
#define MSHC_RINTSTS 0x44
#define MSHC_FIFOTH  0x4c
#define MSHC_DEBNCE  0x64

/* mshc_command flags */
#define MSHC_F_INIT  (1u << 15)  /* 80 clocks before the command (CMD0) */
#define MSHC_F_CRC   (1u << 8)   /* check the response CRC */
#define MSHC_F_LONG  (1u << 7)   /* 136-bit response */
#define MSHC_F_RESP  (1u << 6)   /* a response is expected */

int mshc_command(uint32_t base, uint32_t idx, uint32_t arg, uint32_t flags);
/* Data command moving count blocks of 512 bytes; CMD18 and CMD25 end with an automatic CMD12 */
int mshc_data(uint32_t base, uint32_t idx, uint32_t arg, uint32_t count, uint32_t *buf, int write);
/* Card clock: CLKDIV (0: the controller's input clock, n: input / 2n), sent to the card */
int mshc_clock(uint32_t base, uint32_t div);

#endif
