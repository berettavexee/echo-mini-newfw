/* Core B (the "HiFi" Cortex-M3 of PD_HIGH): power, loading of our codec images (hifi_proto.h)
 * from the pack of our firmware image, start, doorbell, clock (ETUDE §1, §19, §24) */
#ifndef HIFI_H
#define HIFI_H

#include <stdint.h>

enum { HIFI_EIMG = -40, HIFI_EREAD = -41, HIFI_ETIMEOUT = -42, HIFI_EPROTO = -43 };

void hifi_power(void);          /* PD_HIGH on, clocks, core B held in reset (once) */
void hifi_stop(void);           /* core B back in reset */
void hifi_sleep(void);          /* reset and PD_HIGH clocks gated, until hifi_power() */
void hifi_prepare(void);        /* PD_HIGH on, every HiFi block reset, core B held */
/* Our own codec images (hifi_proto.h): loaded from the pack of our firmware image */
int hifi_load_own(uint32_t codec);
int hifi_start_own(void);       /* waits for the control block to say ready */
uint32_t hifi_own_loaded(void); /* our codec whose image is in the HIGHRAM, 0: none */
int hifi_restart_own(void);     /* the same codec again, without reloading it (then start_own) */
void hifi_ring(void);           /* doorbell: core B has something to look at */

/* Core B address to a core A pointer (HIGHRAM0 is seen at 0 by core B) */
static inline void *hifi_ptr(uint32_t a)
{
    return (void *)(a < 0x20000 ? a + 0x01000000 : a);
}

extern uint32_t hifi_data_size, hifi_hz, hifi_div;
void hifi_set_div(uint32_t div);   /* core B at PLL / div (400 MHz / 1..8) */

#endif
