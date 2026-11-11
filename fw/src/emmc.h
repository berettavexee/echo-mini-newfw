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
extern volatile int emmc_busy;  /* inside a transfer: a fault there writes no record */
int emmc_write_fw(uint32_t sector, uint32_t count, const void *buf);
int emmc_sectors(uint32_t *n);  /* card size (EXT_CSD SEC_COUNT) */
/* User volume writes: the area must start after the firmware area; writes outside it fail */
int emmc_user_area(uint32_t base, uint32_t count);
int emmc_write_user(uint32_t sector, uint32_t count, const void *buf);
/* Log file writes: the data sectors of the log file, after the firmware area */
int emmc_log_area(uint32_t base, uint32_t count);
int emmc_write_log(uint32_t sector, uint32_t count, const void *buf);

/* A blob of the firmware image in copy k (0: FW1, 1: FW2): the image's header read into sec (512
 * bytes), checked ("RKnanoFW"); the blob's {offset, length} taken at byte slot of the header; its
 * first sector read into sec, its first word checked against magic. 0 and *lba, *len; or -1.
 * (The font, the languages, the log's formats, the codec pack, the overlays: tools/mkimg.py.) */
static inline int fw_blob(int k, uint32_t slot, uint32_t magic, uint32_t *sec, uint32_t *lba,
                          uint32_t *len)
{
    const uint8_t *b = (const uint8_t *)sec;
    uint32_t img = EMMC_FW1 + (k ? emmc.r : 0);
    if (emmc_read(img, 1, sec) || b[0x1f8] != 'R' || b[0x1ff] != 'W')
        return -1;
    uint32_t off = sec[slot / 4], n = sec[slot / 4 + 1];
    if (!n || off % 512 || emmc_read(img + off / 512, 1, sec) || sec[0] != magic)
        return -1;
    *lba = img + off / 512;
    *len = n;
    return 0;
}

/* Reads counted (ETUDE §126: where the library's build spends its time): commands, sectors,
 * microseconds in the reads; the card's in sd.c */
struct io_count { uint32_t cmds, sectors, us; };
extern struct io_count emmc_io;
extern struct io_count emmc_wio;                /* the writes of the user area */

#endif
