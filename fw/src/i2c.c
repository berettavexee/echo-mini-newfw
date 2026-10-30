/* I2C masters, polled (TRM chapter 22): I2C0 for the two CS43131 DACs, I2C1 (pins C: GPIO2_12
 * SDA, GPIO2_13 SCL) for the real-time clock at 0xA2 (ETUDE §28). Transfers as RECHO 0.10.0
 * drives the CS43131s (its accesses observed in emulation; the controller is programmed here
 * from the TRM):
 *  - register write: START, then one transmit of [dev, reg23:16, reg15:8, reg7:0, 0x01, val],
 *    then STOP;
 *  - register read: the same first five bytes as a write (no value), STOP, then a receive
 *    transaction from dev | 1 of one byte, last byte NAKed, STOP.
 * Every wait is bounded; a NAK or a timeout counts as an error and stops the transfer. */
#include "i2c.h"
#include "board.h"
#include "regs.h"

#define I2C0      0x40040000u
#define I2C1      0x40050000u
static uint32_t bus = I2C0;                     /* the controller of the current transfer */
#define CON       (bus + 0x00)
#define CLKDIV    (bus + 0x04)
#define MRXADDR   (bus + 0x08)
#define MRXRADDR  (bus + 0x0c)
#define MTXCNT    (bus + 0x10)
#define MRXCNT    (bus + 0x14)
#define IEN       (bus + 0x18)
#define IPD       (bus + 0x1c)
#define TXDATA(n) (bus + 0x100 + 4 * (n))
#define RXDATA(n) (bus + 0x200 + 4 * (n))

#define CON_EN    1u
#define CON_TRX   (1u << 1)                     /* mode 1: address, restart, receive */
#define CON_START (1u << 3)
#define CON_STOP  (1u << 4)
#define CON_NAK   (1u << 5)                     /* NAK the last received byte */

#define IPD_MBTF  (1u << 2)
#define IPD_MBRF  (1u << 3)
#define IPD_START (1u << 4)
#define IPD_STOP  (1u << 5)
#define IPD_NAK   (1u << 6)

#define WAIT_US   5000

uint32_t i2c_errors;

static int wait_ipd(uint32_t bit)
{
    uint32_t t0 = ticks();
    for (;;) {
        uint32_t p = REG(IPD);
        if (p & IPD_NAK)
            return I2C_ENAK;
        if (p & bit)
            return 0;
        if (ticks() - t0 > WAIT_US)
            return I2C_ETIMEOUT;
    }
}

static int start(uint32_t mode)
{
    REG(IPD) = 0x7f;
    REG(IEN) = 0;
    REG(CON) = CON_EN | mode | CON_START;
    int r = wait_ipd(IPD_START);
    REG(IPD) = IPD_START;
    REG(CON) = CON_EN | mode;
    return r;
}

static void stop(void)
{
    REG(IPD) = 0x7f;
    REG(CON) = CON_EN | CON_STOP;
    wait_ipd(IPD_STOP);
    REG(IPD) = 0x7f;
    REG(CON) = 0;
}

/* Transmits n (<= 32) bytes, the first one being the device address */
static int transmit(const uint8_t *b, uint32_t n)
{
    int r = start(0);
    if (!r) {
        for (uint32_t i = 0; i < n; i += 4) {
            uint32_t w = 0;
            for (uint32_t k = 0; k < 4 && i + k < n; k++)
                w |= (uint32_t)b[i + k] << (8 * k);
            REG(TXDATA(i / 4)) = w;
        }
        REG(MTXCNT) = n;
        r = wait_ipd(IPD_MBTF);
    }
    stop();
    return r;
}

/* Clock 0x83 (gate 8 bit 3) on, soft reset 0x1a pulsed, GRF +0xbc routing, GPIO2_10 and
 * GPIO2_11 in function 1 (SCL, SDA), SCL from PCLK: (14 + 1) * 8 * 2 PCLK periods. */
void i2c_init(void)
{
    bus = I2C0;
    clk_gate(8 * 16 + 3, 1);
    soft_reset(1 * 16 + 10, 1);
    udelay(100);
    soft_reset(1 * 16 + 10, 0);
    REG(GRF_BASE + 0xbc) = 0x00030000;
    iomux(2, 10, 1);
    iomux(2, 11, 1);
    REG(CON) = 0;
    REG(CLKDIV) = 0x000e000e;
}

int i2c_reg_write(uint8_t dev, uint32_t reg, uint8_t val)
{
    bus = I2C0;
    uint8_t b[6] = { dev, (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 1, val };
    int r = transmit(b, 6);
    if (r)
        i2c_errors++;
    return r;
}

int i2c_reg_read(uint8_t dev, uint32_t reg, uint8_t *val)
{
    bus = I2C0;
    uint8_t b[5] = { dev, (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 1 };
    int r = transmit(b, 5);
    if (!r) {
        REG(MRXADDR) = 1u << 24 | (dev | 1u);
        REG(MRXRADDR) = 0;
        r = start(CON_TRX);
        if (!r) {
            REG(CON) = CON_EN | CON_TRX | CON_NAK;
            REG(MRXCNT) = 1;
            r = wait_ipd(IPD_MBRF);
            if (!r)
                *val = (uint8_t)REG(RXDATA(0));
        }
        stop();
    }
    if (r)
        i2c_errors++;
    return r;
}

/* I2C1 on pins C: clock (gate 8 bit 4) on, soft reset 1.11 pulsed, GRF_IOMUX_CON i2c1_sel = 2,
 * GPIO2_12 / GPIO2_13 in function 3, the same SCL divider as I2C0 */
void i2c1_init(void)
{
    bus = I2C1;
    clk_gate(8 * 16 + 4, 1);
    soft_reset(1 * 16 + 11, 1);
    udelay(100);
    soft_reset(1 * 16 + 11, 0);
    REG(GRF_BASE + 0xbc) = 0x000c0008;
    iomux(2, 12, 3);
    iomux(2, 13, 3);
    REG(CON) = 0;
    REG(CLKDIV) = 0x000e000e;
}

/* n (<= 31) registers from 'reg' on, 8-bit register address: the address written, STOP, then a
 * receive transaction of n bytes, the last one NAKed */
int i2c1_read(uint8_t dev, uint8_t reg, uint8_t *buf, uint32_t n)
{
    bus = I2C1;
    uint8_t b[2] = { dev, reg };
    int r = n && n < 32 ? transmit(b, 2) : I2C_ETIMEOUT;
    if (!r) {
        REG(MRXADDR) = 1u << 24 | (dev | 1u);
        REG(MRXRADDR) = 0;
        r = start(CON_TRX);
        if (!r) {
            REG(CON) = CON_EN | CON_TRX | CON_NAK;
            REG(MRXCNT) = n;
            r = wait_ipd(IPD_MBRF);
            for (uint32_t i = 0; !r && i < n; i++)
                buf[i] = (uint8_t)(REG(RXDATA(i / 4)) >> (8 * (i % 4)));
        }
        stop();
    }
    return r;
}

/* n (<= 30) registers from 'reg' on */
int i2c1_write(uint8_t dev, uint8_t reg, const uint8_t *buf, uint32_t n)
{
    bus = I2C1;
    uint8_t b[32] = { dev, reg };
    if (n > 30)
        return I2C_ETIMEOUT;
    for (uint32_t i = 0; i < n; i++)
        b[2 + i] = buf[i];
    return transmit(b, n + 2);
}
