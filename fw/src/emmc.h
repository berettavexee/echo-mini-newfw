/* Internal eMMC: sector reads, writes restricted to the firmware area, geometry from the IDB */
#ifndef EMMC_H
#define EMMC_H

#include <stdint.h>

#define EMMC_FW1 0x2000u        /* first sector of the first firmware copy */

enum {
    EMMC_ETIMEOUT = -2, EMMC_ECMD = -3, EMMC_EDATA = -4, EMMC_EBUSY = -5,
    EMMC_EPROTECT = -6, EMMC_ENOIDB = -7,
};

struct emmc_geom {
    uint32_t p;                 /* system area, sectors (two firmware copies) */
    uint32_t r;                 /* one firmware copy: FW2 starts at EMMC_FW1 + r */
    int idb_copy;
};

extern struct emmc_geom emmc;

int emmc_init(void);
int emmc_read(uint32_t sector, uint32_t count, void *buf);
int emmc_write_fw(uint32_t sector, uint32_t count, const void *buf);
int emmc_sectors(uint32_t *n);  /* card size (EXT_CSD SEC_COUNT) */
/* User volume writes: the area must start after the firmware area; writes outside it fail */
int emmc_user_area(uint32_t base, uint32_t count);
int emmc_write_user(uint32_t sector, uint32_t count, const void *buf);
/* Log file writes: the data sectors of the log file, after the firmware area */
int emmc_log_area(uint32_t base, uint32_t count);
int emmc_write_log(uint32_t sector, uint32_t count, const void *buf);

#endif
