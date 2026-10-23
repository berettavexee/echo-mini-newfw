/* Firmware installer: any valid RKnanoFW image (stock, RECHO, ours), no version or date check */
#ifndef UPDATE_H
#define UPDATE_H

#include <stdint.h>
#include "fat.h"

enum {
    UPD_EREAD = -10, UPD_ENOTFW = -11, UPD_ECOPY = -12, UPD_ESIZE = -13, UPD_ECRC = -14,
    UPD_EWRITE = -15, UPD_EVERIFY = -16,
};

struct fw_info {
    uint32_t fw_end;            /* header copy offset */
    uint32_t fw_size;           /* bytes written to a copy, CRC stored right after */
    uint16_t year;              /* BCD date of the header */
    uint8_t month, day;
    int two_copies;             /* fits in one copy: FW2 is written too */
};

/* progress(stage, done, total): stage 0 = CRC check, 1 = FW2, 2 = FW1 */
typedef void (*progress_fn)(int stage, uint32_t done, uint32_t total);

int fw_check(struct fat_file *f, struct fw_info *info, progress_fn progress);
int fw_install(struct fat_file *f, const struct fw_info *info, progress_fn progress);
const char *upd_error(int e);

#endif
