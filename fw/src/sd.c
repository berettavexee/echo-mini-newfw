/* microSD card on the DesignWare host at 0x60020000 (mshc.c). Set-up as in the stock firmware
 * (ETUDE §36: 0x0306048c, 0x03060308): pins GPIO1A5-1A7 and GPIO1B0-1B2 in function 1 (CMD,
 * CLK, D0, D1-D3), clock gates 24 (clk_sdmmc) and 107 (hclk_sdmmc), soft reset 17, card detect
 * GPIO0_11 (low: a card), no power switch. The controller clock comes from the 24 MHz crystal
 * (CRU_CLKSEL7 bit 7, divider 1): it does not follow the PLL, which the firmware changes. The
 * card is set up by the SD 2.0 sequence (simplified physical layer specification, public): CMD0,
 * CMD8, ACMD41, CMD2, CMD3, CMD9, CMD7, ACMD6, then 24 MHz on 4 bits (default speed). */
#include "sd.h"
#include "board.h"
#include "log.h"
#include "mshc.h"
#include "regs.h"

#define CRU_CLKSEL7 (CRU_BASE + 0x30)
#define B           MSHC_SD

#define R1  (MSHC_F_RESP | MSHC_F_CRC)
#define R2  (MSHC_F_RESP | MSHC_F_LONG | MSHC_F_CRC)
#define R3  MSHC_F_RESP                         /* OCR: no CRC */

struct sd_card sd;

int sd_present(void)
{
    return !gpio_get(0, 11);
}

static int acmd(uint32_t idx, uint32_t arg, uint32_t flags)
{
    int r = mshc_command(B, 55, sd.rca << 16, R1);
    return r ? r : mshc_command(B, idx, arg, flags);
}

static void host_setup(void)
{
    for (int p = 5; p <= 10; p++)               /* GPIO1A5..1A7, 1B0..1B2 */
        iomux(1, p, 1);
    gpio_pull(0, 11, 1);
    clk_gate(24, 1);
    clk_gate(107, 1);
    soft_reset(17, 1);
    udelay(10);
    soft_reset(17, 0);
    REG(CRU_CLKSEL7) = 0x009f0080u;             /* 24 MHz crystal, divided by 1 */
    REG(B + MSHC_CTRL) = 7;                     /* controller, FIFO and DMA reset */
    for (uint32_t t0 = ticks(); (REG(B + MSHC_CTRL) & 7) && ticks() - t0 < 10000;)
        ;
    REG(B + MSHC_FIFOTH) = 0x300f0010u;
    REG(B + MSHC_CTYPE) = 0;
    REG(B + MSHC_TMOUT) = 0xffffff40u;
    REG(B + MSHC_RINTSTS) = 0xffffffffu;
    REG(B + MSHC_INTMASK) = 0;                  /* polled */
    REG(B + MSHC_PWREN) = 1;
}

int sd_init(void)
{
    int r;
    sd.ready = sd.hc = 0;
    sd.rca = sd.sectors = sd.ocr = 0;
    if (!sd_present())
        return EMMC_ENOIDB;
    host_setup();
    if ((r = mshc_clock(B, 30)))                /* 24 MHz / 60 = 400 kHz for the identification */
        goto fail;
    mdelay(2);
    mshc_command(B, 0, 0, MSHC_F_INIT);         /* GO_IDLE_STATE */
    int v2 = !mshc_command(B, 8, 0x1aa, R1) && (REG(B + MSHC_RESP(0)) & 0xfff) == 0x1aa;
    uint32_t t0 = ticks();
    do {                                        /* SD_SEND_OP_COND until the card is ready */
        if ((r = acmd(41, (v2 ? 0x40000000u : 0) | 0x00ff8000u, R3)))
            goto fail;
        sd.ocr = REG(B + MSHC_RESP(0));
        if (ticks() - t0 > 1000000) {
            r = EMMC_ETIMEOUT;
            goto fail;
        }
    } while (!(sd.ocr & 0x80000000u));
    sd.hc = (sd.ocr >> 30) & 1;
    if ((r = mshc_command(B, 2, 0, R2)))        /* ALL_SEND_CID */
        goto fail;
    if ((r = mshc_command(B, 3, 0, R1)))        /* SEND_RELATIVE_ADDR */
        goto fail;
    sd.rca = REG(B + MSHC_RESP(0)) >> 16;
    if ((r = mshc_command(B, 9, sd.rca << 16, R2)))   /* SEND_CSD */
        goto fail;
    uint32_t c0 = REG(B + MSHC_RESP(0)), c1 = REG(B + MSHC_RESP(1)), c2 = REG(B + MSHC_RESP(2)),
             c3 = REG(B + MSHC_RESP(3));        /* bit k of the CSD: RESP(k / 32) bit k % 32 */
    (void)c0;
    if (c3 >> 30 == 1)                          /* CSD 2.0: (C_SIZE [69:48] + 1) * 512 KiB */
        sd.sectors = ((c1 >> 16 | (c2 & 0x3f) << 16) + 1) * 1024;
    else {                                      /* CSD 1.0 */
        uint32_t c_size = (c1 >> 30 | (c2 & 0x3ff) << 2) + 1, mult = (c1 >> 15) & 7,
                 bl_len = (c2 >> 16) & 15;
        sd.sectors = c_size << (mult + 2 + bl_len - 9);
    }
    if ((r = mshc_command(B, 7, sd.rca << 16, R1)))   /* SELECT_CARD */
        goto fail;
    if ((r = acmd(6, 2, R1)))                   /* SET_BUS_WIDTH: 4 bits */
        goto fail;
    REG(B + MSHC_CTYPE) = 1;
    if (!sd.hc && (r = mshc_command(B, 16, 512, R1)))
        goto fail;
    if ((r = mshc_clock(B, 0)))                 /* 24 MHz */
        goto fail;
    sd.ready = 1;
    log_printf("sd: %s card, %u MiB, OCR %x, RCA %x", sd.hc ? "SDHC/SDXC" : "SDSC",
               sd.sectors / 2048, sd.ocr, sd.rca);
    return 0;
fail:
    log_printf("sd: set-up failed (%d), OCR %x", r, sd.ocr);
    return r;
}

int sd_read(uint32_t sector, uint32_t count, void *buf)
{
    if (!sd.ready)
        return EMMC_EBUSY;
    while (count) {
        uint32_t n = count > 64 ? 64 : count;
        int r = mshc_data(B, n > 1 ? 18 : 17, sd.hc ? sector : sector * 512, n, buf, 0);
        if (r)
            return r;
        sector += n;
        count -= n;
        buf = (uint8_t *)buf + 512 * n;
    }
    return 0;
}
