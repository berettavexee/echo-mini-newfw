/* microSD card (ETUDE §36): SD 2.0 initialisation, 4-bit bus at 24 MHz, sector reads */
#ifndef SD_H
#define SD_H

#include <stdint.h>

struct sd_card {
    int ready;
    int hc;                     /* SDHC/SDXC: block addressing */
    uint32_t sectors;
    uint32_t rca;
    uint32_t ocr;
};

extern struct sd_card sd;

int sd_present(void);           /* the card-detect switch (GPIO0_11, low: a card) */
int sd_init(void);              /* 0, or EMMC_E* (mshc.h); logs what it finds */
int sd_read(uint32_t sector, uint32_t count, void *buf);

#endif
