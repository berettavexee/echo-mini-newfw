/* DesignWare SD/MMC host (TRM chapters 10-11), shared by the eMMC (emmc.c) and the microSD card
 * (sd.c): commands, data through the 16-word FIFO, card clock. Moved out of emmc.c in 0.56. */
#include "mshc.h"
#include "board.h"
#include "regs.h"

#define CTRL       0x00
#define BLKSIZ     0x1c
#define BYTCNT     0x20
#define CMDARG     0x28
#define CMD        0x2c
#define RINTSTS    0x44
#define STATUS     0x48
#define FIFO       0x200

#define CMD_START      (1u << 31)
#define CMD_HOLD       (1u << 29)
#define CMD_UPDATE_CLK (1u << 21)
#define CMD_WAIT_PREV  (1u << 13)
#define CMD_AUTO_STOP  (1u << 12)
#define CMD_WRITE      (1u << 10)
#define CMD_DATA       (1u << 9)
#define CMD_CRC        (1u << 8)
#define CMD_RESP       (1u << 6)

#define INT_RE   (1u << 1)      /* response error */
#define INT_CD   (1u << 2)      /* command done */
#define INT_DTO  (1u << 3)      /* data transfer over */
#define INT_RCRC (1u << 6)
#define INT_DCRC (1u << 7)
#define INT_RTO  (1u << 8)
#define INT_DRTO (1u << 9)
#define INT_HTO  (1u << 10)
#define INT_FRUN (1u << 11)
#define INT_HLE  (1u << 12)
#define INT_SBE  (1u << 13)
#define INT_ACD  (1u << 14)
#define INT_EBE  (1u << 15)
#define INT_CMD_ERR  (INT_RE | INT_RCRC | INT_RTO | INT_HLE)
#define INT_DATA_ERR (INT_DCRC | INT_DRTO | INT_HTO | INT_FRUN | INT_SBE | INT_EBE)

#define ST_FIFO_EMPTY  (1u << 2)
#define ST_DATA_BUSY   (1u << 9)
#define ST_FIFO_COUNT(s) (((s) >> 17) & 0x1fff)
#define FIFO_WORDS 16

#define TIMEOUT_US 500000

static int wait_clear(uint32_t reg, uint32_t mask, uint32_t us)
{
    uint32_t t0 = ticks();
    while (REG(reg) & mask) {
        if (ticks() - t0 > us)
            return -1;
        wdt_kick();
    }
    return 0;
}

/* Card ready for a new command: no data transfer, card not busy programming */
static int idle(uint32_t b)
{
    if (wait_clear(b + STATUS, ST_DATA_BUSY, TIMEOUT_US))
        return EMMC_EBUSY;
    if (!(REG(b + STATUS) & ST_FIFO_EMPTY)) {
        REG(b + CTRL) |= 2;                     /* FIFO reset */
        if (wait_clear(b + CTRL, 2, 10000))
            return EMMC_EBUSY;
    }
    return 0;
}

int mshc_command(uint32_t b, uint32_t idx, uint32_t arg, uint32_t flags)
{
    REG(b + RINTSTS) = 0xffffffff;
    REG(b + CMDARG) = arg;
    REG(b + CMD) = CMD_START | CMD_HOLD | CMD_WAIT_PREV | flags | idx;
    if (wait_clear(b + CMD, CMD_START, TIMEOUT_US))
        return EMMC_ETIMEOUT;
    uint32_t t0 = ticks();
    while (!(REG(b + RINTSTS) & INT_CD))
        if (ticks() - t0 > TIMEOUT_US)
            return EMMC_ETIMEOUT;
    if (REG(b + RINTSTS) & INT_CMD_ERR)
        return EMMC_ECMD;
    return 0;
}

/* Wait for the end of the data phase (and of the automatic CMD12 when one was sent) */
static int data_done(uint32_t b, int auto_stop)
{
    uint32_t want = INT_DTO | (auto_stop ? INT_ACD : 0), t0 = ticks();
    for (;;) {
        uint32_t s = REG(b + RINTSTS);
        if (s & INT_DATA_ERR)
            return EMMC_EDATA;
        if ((s & want) == want)
            return 0;
        if (ticks() - t0 > TIMEOUT_US)
            return EMMC_ETIMEOUT;
    }
}

int mshc_data(uint32_t b, uint32_t idx, uint32_t arg, uint32_t count, uint32_t *buf, int write)
{
    int r, multi = idx == 18 || idx == 25;
    uint32_t words = count * 128;

    if ((r = idle(b)))
        return r;
    REG(b + BLKSIZ) = 512;
    REG(b + BYTCNT) = count * 512;
    r = mshc_command(b, idx, arg, CMD_RESP | CMD_CRC | CMD_DATA | (write ? CMD_WRITE : 0) | (multi ? CMD_AUTO_STOP : 0));
    if (r)
        return r;
    uint32_t t0 = ticks();
    while (words) {
        uint32_t s = REG(b + STATUS), n;
        if (REG(b + RINTSTS) & INT_DATA_ERR)
            return EMMC_EDATA;
        if (write)
            n = FIFO_WORDS - ST_FIFO_COUNT(s);
        else
            n = ST_FIFO_COUNT(s);
        if (n > words)
            n = words;
        if (!n) {
            if (ticks() - t0 > TIMEOUT_US)
                return EMMC_ETIMEOUT;
            continue;
        }
        t0 = ticks();
        words -= n;
        if (write)
            while (n--)
                REG(b + FIFO) = *buf++;
        else
            while (n--)
                *buf++ = REG(b + FIFO);
    }
    r = data_done(b, multi);
    if (!r && write)                            /* card programming */
        r = wait_clear(b + STATUS, ST_DATA_BUSY, 2000000) ? EMMC_EBUSY : 0;
    return r;
}


/* The clock change is sent to the card clock domain with an "update clock" command */
static int clock_update(uint32_t b)
{
    REG(b + CMD) = CMD_START | CMD_UPDATE_CLK | CMD_WAIT_PREV;
    return wait_clear(b + CMD, CMD_START, TIMEOUT_US) ? EMMC_ETIMEOUT : 0;
}

int mshc_clock(uint32_t b, uint32_t div)
{
    int r;
    REG(b + MSHC_CLKENA) = 0;
    if ((r = clock_update(b)))
        return r;
    REG(b + MSHC_CLKDIV) = div;
    if ((r = clock_update(b)))
        return r;
    REG(b + MSHC_CLKENA) = 1;
    return clock_update(b);
}
