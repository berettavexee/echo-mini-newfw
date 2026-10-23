/* USB mass storage (bulk only transport, SCSI transparent command set) exposing the user
 * volume of the eMMC: sectors [base, base + count), read only or read/write. */
#ifndef MSC_H
#define MSC_H

#include <stdint.h>

struct msc_state {
    uint32_t base, count;       /* exposed sectors */
    int writable;
    uint32_t commands, read_sectors, written_sectors, errors;
    uint8_t last_op;
    int ejected;                /* START STOP UNIT with eject */
};

extern struct msc_state msc;

int msc_start(uint32_t base, uint32_t count, int writable);  /* <0: write fence refused */
void msc_poll(void);            /* one step; call in a loop with usb_service() */

#endif
