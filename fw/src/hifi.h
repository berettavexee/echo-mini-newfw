/* Core B (the "HiFi" Cortex-M3 of PD_HIGH): power, loading of a stock codec module from a stock
 * firmware image on the user volume, start, and the mailbox (0x40110000) used by the stock
 * codec runtime (ETUDE §1). The modules are read from the user's own image at run time and are
 * never part of our firmware. */
#ifndef HIFI_H
#define HIFI_H

#include <stdint.h>
#include "fat.h"

enum { HIFI_EIMG = -40, HIFI_EREAD = -41, HIFI_ETIMEOUT = -42, HIFI_EPROTO = -43 };

void hifi_power(void);          /* PD_HIGH on, clocks, core B held in reset (once) */
/* Finds a stock image in the root (an .IMG whose overlay 'module' is a HiFi module) */
int hifi_find_image(int module, struct fat_file *img);
/* Core B in reset, overlay 'module' of 'img' copied to HIGHRAM0 (code) and HIGHRAM1 (data) */
int hifi_load(const struct fat_file *img, int module);
int hifi_start(void);           /* parameter block, reset released; waits for "ready" (1) */
void hifi_stop(void);           /* core B back in reset */
void hifi_sleep(void);          /* reset and PD_HIGH clocks gated, until hifi_power() */
void hifi_dump(const char *why);   /* mailbox registers into the log */
void hifi_prepare(void);        /* PD_HIGH on, every HiFi block reset, core B held */
/* Our own codec images (hifi_proto.h): loaded from the pack of our firmware image */
int hifi_load_own(uint32_t codec);
int hifi_start_own(void);       /* waits for the control block to say ready */
uint32_t hifi_own_loaded(void); /* our codec whose image is in the HIGHRAM, 0: none */
int hifi_restart_own(void);     /* the same codec again, without reloading it (then start_own) */
void hifi_ring(void);           /* doorbell: core B has something to look at */

void hifi_send(int ch, uint32_t cmd, uint32_t data);           /* A to B */
int hifi_poll(int *ch, uint32_t *cmd, uint32_t *data);         /* B to A: 1 = one message */
/* Core B address to a core A pointer (HIGHRAM0 is seen at 0 by core B) */
static inline void *hifi_ptr(uint32_t a)
{
    return (void *)(a < 0x20000 ? a + 0x01000000 : a);
}

extern uint32_t hifi_code_size, hifi_data_size, hifi_hz, hifi_div;
void hifi_set_div(uint32_t div);   /* core B at PLL / div (400 MHz / 1..8) */

#endif
