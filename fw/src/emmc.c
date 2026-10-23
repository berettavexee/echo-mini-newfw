/* eMMC on the DesignWare SD/MMC controller at 0x60030000 (mshc.c), data moved by the CPU through
 * the 16-word FIFO (TRM chapters 10-11).
 *
 * The IDB loader has already initialised the card to read us (clock, bus width, transfer
 * state, sector addressing), so the controller and the card are used as they are, as RECHO
 * does. Writes go through two entry points, each with its own fence:
 *  - emmc_write_fw: inside the firmware area [FW1, FW1 + P) known from the IDB (installer);
 *  - emmc_write_user: inside the user volume declared by emmc_user_area(), which must lie
 *    entirely after the firmware area (USB disk in read/write mode);
 *  - emmc_write_log: inside the sectors of the log file declared by emmc_log_area(), also
 *    after the firmware area (data sectors of one preallocated file, nothing else).
 * Nothing below FW1 (the IDB) can ever be written. */
#include "emmc.h"
#include "mshc.h"

struct emmc_geom emmc;

static int transfer(uint32_t sector, uint32_t count, uint32_t *buf, int write)
{
    int multi = count > 1;
    return mshc_data(MSHC_EMMC, write ? (multi ? 25 : 24) : (multi ? 18 : 17), sector, count, buf,
                     write);
}

/* Card size in sectors, from SEC_COUNT (bytes 212-215) of the EXT_CSD (CMD8, read only) */
int emmc_sectors(uint32_t *n)
{
    static uint32_t ext[128];
    int r = mshc_data(MSHC_EMMC, 8, 0, 1, ext, 0);
    if (r)
        return r;
    const uint8_t *b = (const uint8_t *)ext;
    *n = b[212] | b[213] << 8 | b[214] << 16 | (uint32_t)b[215] << 24;
    return *n ? 0 : EMMC_EDATA;
}

int emmc_read(uint32_t sector, uint32_t count, void *buf)
{
    while (count) {
        uint32_t n = count > 64 ? 64 : count;
        int r = transfer(sector, n, buf, 0);
        if (r)
            return r;
        sector += n;
        count -= n;
        buf = (uint8_t *)buf + 512 * n;
    }
    return 0;
}

/* Multi-block writes of up to 128 sectors: the card programs large runs much faster */
static int write_run(uint32_t sector, uint32_t count, const void *buf)
{
    while (count) {
        uint32_t n = count > 128 ? 128 : count;
        int r = transfer(sector, n, (uint32_t *)buf, 1);
        if (r)
            return r;
        sector += n;
        count -= n;
        buf = (const uint8_t *)buf + 512 * n;
    }
    return 0;
}

int emmc_write_fw(uint32_t sector, uint32_t count, const void *buf)
{
    if (!emmc.p || sector < EMMC_FW1 || sector + count > EMMC_FW1 + emmc.p || sector + count < sector)
        return EMMC_EPROTECT;
    return write_run(sector, count, buf);
}

static uint32_t user_lo, user_hi;

int emmc_user_area(uint32_t base, uint32_t count)
{
    user_lo = user_hi = 0;
    if (!emmc.p || base < EMMC_FW1 + emmc.p || base + count < base || !count)
        return EMMC_EPROTECT;
    user_lo = base;
    user_hi = base + count;
    return 0;
}

int emmc_write_user(uint32_t sector, uint32_t count, const void *buf)
{
    if (user_hi <= user_lo || user_lo < EMMC_FW1 + emmc.p || sector < user_lo ||
        sector + count > user_hi || sector + count < sector)
        return EMMC_EPROTECT;
    return write_run(sector, count, buf);
}

/* IDB: 5 copies, sector 64 + 1024 * i. Sector 0 of a copy starts with 0xfcdc8c3b, sector 1 has
 * "NANO" at +0x0a and the size of the system area in MiB (u16) at +0x02. P = MiB * 2048
 * sectors; each firmware copy gets P / 2 = R, FW1 at 0x2000, FW2 at 0x2000 + R. */
int emmc_init(void)
{
    static uint32_t s[256];
    int r = -1;

    for (int i = 0; i < 5; i++) {
        if ((r = emmc_read(64 + 1024 * i, 2, s)))
            continue;
        const uint8_t *b = (const uint8_t *)s;
        if (s[0] != 0xfcdc8c3b || b[512 + 0x0a] != 'N' || b[512 + 0x0b] != 'A' ||
            b[512 + 0x0c] != 'N' || b[512 + 0x0d] != 'O')
            continue;
        uint32_t mib = b[512 + 2] | b[512 + 3] << 8;
        if (mib < 8 || mib > 1024)
            continue;
        emmc.p = mib * 2048;
        emmc.r = emmc.p / 2;
        emmc.idb_copy = i;
        return 0;
    }
    return r ? r : EMMC_ENOIDB;
}

static uint32_t log_lo, log_hi;

int emmc_log_area(uint32_t base, uint32_t count)
{
    log_lo = log_hi = 0;
    if (!emmc.p || base < EMMC_FW1 + emmc.p || base + count < base || !count)
        return EMMC_EPROTECT;
    log_lo = base;
    log_hi = base + count;
    return 0;
}

int emmc_write_log(uint32_t sector, uint32_t count, const void *buf)
{
    if (log_hi <= log_lo || log_lo < EMMC_FW1 + emmc.p || sector < log_lo ||
        sector + count > log_hi || sector + count < sector)
        return EMMC_EPROTECT;
    return write_run(sector, count, buf);
}
