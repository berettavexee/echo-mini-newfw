/* Board support: pins, clocks, delays, keys, ADC, power. */
#include <string.h>
#include "board.h"
#include "regs.h"

static const uint32_t gpio_base[3] = { GPIO0_BASE, GPIO1_BASE, GPIO2_BASE };

void iomux(int bank, int pin, int func)
{
    int sh = 2 * (pin & 7);
    REG(GRF_IOMUX(bank, pin)) = (3u << (16 + sh)) | ((uint32_t)func << sh);
}

void gpio_dir(int bank, int pin, int out)
{
    volatile uint32_t *r = (volatile uint32_t *)(gpio_base[bank] + GPIO_DDR);
    *r = out ? *r | (1u << pin) : *r & ~(1u << pin);
}

void gpio_pull(int bank, int pin, int on)
{
    uint32_t bit = 1u << (pin & 7);
    REG(GRF_PULL(bank, pin)) = bit << 16 | (on ? 0 : bit);
}

void gpio_set(int bank, int pin, int val)
{
    volatile uint32_t *r = (volatile uint32_t *)(gpio_base[bank] + GPIO_DR);
    *r = val ? *r | (1u << pin) : *r & ~(1u << pin);
}

int gpio_get(int bank, int pin)
{
    return (REG(gpio_base[bank] + GPIO_EXT) >> pin) & 1;
}

/* Clock gates and soft resets are numbered 16 * register + bit. A gate bit at 1 stops the clock.
 * CLKGATE7 is read back and written with a full mask, as RECHO does for that register only. */
void clk_gate(int n, int on)
{
    uint32_t bit = 1u << (n & 15);

    if (n >> 4 == 7) {
        uint32_t v = REG(CRU_CLKGATE(7)) & 0xffff;
        REG(CRU_CLKGATE(7)) = 0xffff0000u | (on ? v & ~bit : v | bit);
        return;
    }
    REG(CRU_CLKGATE(n >> 4)) = bit << 16 | (on ? 0 : bit);
}

void soft_reset(int n, int on)
{
    REG(CRU_SOFTRST(n >> 4)) = (1u << (16 + (n & 15))) | ((on ? 1u : 0u) << (n & 15));
}

/* ---- clocks ---- */
uint32_t cpu_hz = 24000000;

/* PLL at 400 MHz (24 MHz / 6 * 100), core and HiFi core at PLL / 2 = 200 MHz, PCLK at 50 MHz.
 * Same register values as RECHO 0.10.0, which logs them on this device. A marker in PMU-SRAM
 * is set during the attempt and cleared by clock_fast_ok() once the firmware runs: if it is
 * still there at boot, the previous 200 MHz boot did not get that far and we stay at 24 MHz. */
/* Core cycles (SysTick, no interrupt) taken by 32 ADC conversions. The converter has its own
 * clock, so the ratio of two counts gives the core frequency: the 0.1 image assumed 200 MHz
 * and its times ran about 4 times too slow on the device. */
uint32_t adc_cycles_24m, adc_cycles, pll_con1;

uint32_t loop_24m, loop_pll, cru_mode, cru_sel0, cru_sel1, mode_tries, pll_con1_after;

static void spin(uint32_t n)
{
    __asm volatile ("1: subs %0, %0, #1\n bne 1b" : "+r"(n));
}     /* SysTick count over a fixed register-only loop */

static void adc_32(void)
{
    for (int n = 0; n < 32; n++) {
        REG(SARADC_CTRL) &= ~0x10u;
        REG(SARADC_CTRL) = (REG(SARADC_CTRL) & ~7u) | 0x10u | 1u;
        for (int i = 0; i < 1000 && !(REG(SARADC_STAS) & 1); i++)
            ;
        for (int i = 0; i < 1000000 && (REG(SARADC_STAS) & 1); i++)
            ;
    }
}

/* SysTick count (CLKSOURCE = core) over 32 ADC conversions, which have their own clock, and
 * over 100000 turns of a register-only loop, whose length in core cycles does not depend on the
 * clock. On the device the DWT cycle counter and the STCLK reference both read 0. If SysTick
 * follows the core, the loop count stays the same on the PLL; if the core is faster than the
 * SysTick clock, it drops by that ratio. */
static uint32_t adc_cycles_count(uint32_t *loop)
{
    REG(SYST_CSR) = 0;
    REG(SYST_RVR) = 0xffffff;
    REG(SYST_CVR) = 0;
    REG(SYST_CSR) = 5;
    uint32_t t0 = REG(SYST_CVR);
    adc_32();
    uint32_t t1 = REG(SYST_CVR);
    uint32_t n = 100000;
    __asm volatile ("1: subs %0, %0, #1\n bne 1b" : "+r"(n));
    uint32_t t2 = REG(SYST_CVR);
    REG(SYST_CSR) = 0;
    *loop = (t1 - t2) & 0xffffff;
    return (t0 - t1) & 0xffffff;
}

int clock_init(void)
{
    volatile uint32_t *mark = (volatile uint32_t *)PMUSRAM_FAST_MARK;

    REG(CRU_MODE_CON) = 0x00010000;             /* core on the 24 MHz crystal */
    REG(CRU_CLKSEL0) = 0xffff0000;              /* all dividers at 1 */
    REG(CRU_CLKSEL1) = 0x03000000;
    cpu_hz = 24000000;
    adc_cycles_24m = adc_cycles_count(&loop_24m);
    if (*mark == FAST_MAGIC)
        return 0;
    *mark = FAST_MAGIC;

    REG(CRU_PLL_CON1) = 0x80008000;
    REG(CRU_PLL_CON1) = 0x40004000;             /* PLL powered down while reprogrammed */
    REG(CRU_PLL_CON2) = 0x08000000;
    REG(CRU_PLL_CON0) = 0xffff1064;
    REG(CRU_PLL_CON1) = 0x11ff1046;
    REG(CRU_PLL_CON2) = 0x08000000;
    REG(CRU_PLL_CON1) = 0x40000000;             /* power up */
    for (uint32_t n = 0; !(REG(CRU_PLL_CON1) & PLL_LOCK); n++)
        if (n > 2400000)                        /* RECHO's bound, a few 100 ms: give up */
            return 0;
    pll_con1 = REG(CRU_PLL_CON1);

    REG(GRF_INTER_CON0) = 0xf0403040;           /* bits 14/15: core B RAMs without wait (§51) */
    /* The stock firmware and RECHO both wait a little after each divider change. Without the
     * waits, 0.1 to 0.5 read MODE_CON back as 0: the core stayed on the crystal, at 24 / 2. */
    REG(CRU_CLKSEL0) = 0xffff1001;              /* core / 2, HiFi core / 2 */
    spin(2000);
    REG(CRU_CLKSEL1) = 0x03000200;              /* PCLK = core / 4 */
    spin(2000);
    for (mode_tries = 1; mode_tries <= 10; mode_tries++) {
        REG(CRU_MODE_CON) = 0x00010001;         /* core on the PLL */
        spin(2000);
        if (REG(CRU_MODE_CON) & 1)
            break;
    }
    pll_con1_after = REG(CRU_PLL_CON1);
    cpu_hz = 200000000;
    adc_cycles = adc_cycles_count(&loop_pll);
    cru_mode = REG(CRU_MODE_CON) & 0xffff;
    cru_sel0 = REG(CRU_CLKSEL0) & 0xffff;
    cru_sel1 = REG(CRU_CLKSEL1) & 0xffff;
    /* Frequency from the registers read back: (PLL 400 MHz or crystal) / (sys_core_div + 1).
     * The ADC count (shown on screen) read 17 MHz for a stopwatch-measured 12 MHz, too rough
     * to drive the timing. */
    cpu_hz = ((cru_mode & 1) ? 400000000u : 24000000u) / ((cru_sel0 & 7) + 1);
    return 1;
}

/* A long pause with the screen off (ui.c): the core and the buses on the 24 MHz crystal (core B
 * too: it follows the same switch) and the PLL powered down; then back as clock_init() left
 * them. The peripherals keep their dividers: from the crystal they run slower, as in the 24 MHz
 * fallback, where they all work. Only from the PLL state. */
int clock_is_low;
static uint32_t low_sel0, low_sel1;

void clock_low(int on)
{
    if (on == clock_is_low || (on && cpu_hz <= 24000000))
        return;                                 /* already on the crystal (the fallback) */
    __asm volatile ("cpsid i");
    if (on) {
        low_sel0 = REG(CRU_CLKSEL0) & 0xffff;
        low_sel1 = REG(CRU_CLKSEL1) & 0xffff;
        REG(CRU_MODE_CON) = 0x00010000;
        spin(2000);
        REG(CRU_CLKSEL0) = 0xffff0000 | (low_sel0 & 0x7000);    /* core B divider kept */
        spin(2000);
        REG(CRU_CLKSEL1) = 0x03000000;
        spin(2000);
        REG(CRU_PLL_CON1) = 0x40004000;         /* PLL powered down */
        cpu_hz = 24000000;
    } else {
        REG(CRU_PLL_CON1) = 0x40000000;
        for (uint32_t n = 0; !(REG(CRU_PLL_CON1) & PLL_LOCK); n++)
            if (n > 2400000)
                break;
        REG(CRU_CLKSEL0) = 0xffff0000 | low_sel0;
        spin(2000);
        REG(CRU_CLKSEL1) = 0xffff0000 | low_sel1;
        spin(2000);
        if (REG(CRU_PLL_CON1) & PLL_LOCK)
            for (int i = 0; i < 10 && !(REG(CRU_MODE_CON) & 1); i++) {
                REG(CRU_MODE_CON) = 0x00010001;
                spin(2000);
            }
        cpu_hz = ((REG(CRU_MODE_CON) & 1) ? 400000000u : 24000000u) / ((low_sel0 & 7) + 1);
    }
    clock_is_low = on;
    timer_init();                               /* SysTick: 1 ms at the new clock */
    __asm volatile ("cpsie i");
}

void clock_fast_ok(void)
{
    *(volatile uint32_t *)PMUSRAM_FAST_MARK = 0;
}

/* ---- time: SysTick at the core clock, 1 ms interrupt ---- */
static volatile uint32_t ms_count;

void SysTick_Handler(void)
{
    ms_count++;
}

void timer_init(void)
{
    REG(SYST_CSR) = 0;
    REG(SYST_RVR) = cpu_hz / 1000 - 1;
    REG(SYST_CVR) = 0;
    REG(SYST_CSR) = 7;                          /* core clock, interrupt, enable */
}

void timer24_init(void)
{
    clk_gate(8 * 16 + 14, 1);                   /* pclk_timer */
    clk_gate(0 * 16 + 12, 1);                   /* clk_timer1 */
    REG(CRU_BASE + 0x1c) = 0x9f000000;          /* CLKSEL2: timer1 from 24 MHz, divider 1 */
    soft_reset(1 * 16 + 5, 1);
    udelay(2);
    soft_reset(1 * 16 + 5, 0);
    REG(0x400e0030) = 0;                        /* TIMER1_CONTROLREG: off */
    REG(0x400e0020) = 0xffffffffu;              /* load count, both halves */
    REG(0x400e0024) = 0xffffffffu;
    REG(0x400e0030) = 1;                        /* on, free running, interrupt masked */
}

uint32_t millis(void)
{
    return ms_count;
}

uint32_t ticks(void)
{
    uint32_t ms, v;
    do {
        ms = ms_count;
        v = REG(SYST_CVR);
    } while (ms != ms_count);
    return ms * 1000 + (REG(SYST_RVR) - v) / (cpu_hz / 1000000);
}

void udelay(uint32_t us)
{
    uint32_t t0 = ticks();
    while (ticks() - t0 < us)
        ;
}

void mdelay(uint32_t ms)
{
    while (ms--) {
        udelay(1000);
        wdt_kick();
    }
}

void wdt_kick(void)
{
    REG(WDT_CRR) = WDT_KICK;
}

/* ---- ADC and keys ---- */
/* Same sequence as the IDB loader: powering the converter up with a channel starts a
 * conversion. Wait for busy to rise, then to fall: at 200 MHz the first poll comes before
 * the conversion has started, and 0.6 read the previous channel's result (battery 4995 mV
 * from the idle key ladder at 1023). */
unsigned adc_read(int ch)
{
    REG(SARADC_CTRL) &= ~0x10u;
    REG(SARADC_CTRL) = (REG(SARADC_CTRL) & ~7u) | 0x10u | (unsigned)ch;
    uint32_t t0 = ticks();
    while (!(REG(SARADC_STAS) & 1) && ticks() - t0 < 200)
        ;
    while ((REG(SARADC_STAS) & 1) && ticks() - t0 < 1000)
        ;
    return REG(SARADC_DATA) & 0x3ff;
}

/* Key ladder on channel 1, medians measured on the device: VOL- 1, VOL+ 126, Play 270,
 * Menu 384, Back 515, none 1023. Thresholds halfway between them. */
int key_ladder(unsigned v)
{
    if (v < 64)
        return KEY_VOLDOWN;
    if (v < 198)
        return KEY_VOLUP;
    if (v < 327)
        return KEY_PLAY;
    if (v < 450)
        return KEY_MENU;
    if (v < 770)
        return KEY_BACK;
    return KEY_NONE;
}

int key_read(void)
{
    return key_ladder(adc_read(1));
}

int power_key(void)
{
    return gpio_get(PIN_POWER_KEY);
}

int vbus(void)
{
    return (REG(GRF_SOC_STATUS) & GRF_VBUS) != 0;
}

/* The charger pulls PIN_CHARGING low while it charges; high when full or without USB (0.40 log) */
int charging(void)
{
    return vbus() && !gpio_get(PIN_CHARGING);
}

/* Channel 0 through a 2:1 divider, 2.5 V reference */
unsigned battery_mv(void)
{
    return adc_read(0) * 5000 / 1024;
}

/* Charge in %, from the voltage: the curve measured on the device during playback (ETUDE §65:
 * a full discharge, 15.8 h, the share of the time left at each voltage) */
#define BAT_POINTS 14
unsigned battery_pct(unsigned v)
{
    static const uint16_t mv[BAT_POINTS] = { 3300, 3400, 3450, 3500, 3550, 3600, 3650, 3700, 3750,
                                             3800, 3900, 4000, 4100, 4160 };
    static const uint8_t pc[BAT_POINTS] = { 0, 3, 8, 13, 22, 36, 50, 59, 65, 70, 79, 89, 96, 100 };
    if (v >= mv[BAT_POINTS - 1])
        return 100;
    for (int i = 1; i < BAT_POINTS; i++)
        if (v < mv[i])
            return v <= mv[i - 1] ? pc[i - 1] :
                   pc[i - 1] + (v - mv[i - 1]) * (pc[i] - pc[i - 1]) / (mv[i] - mv[i - 1]);
    return 0;
}

/* ---- reset and power ---- */
/* Back to the crystal before any reset: SYSRESETREQ does not reset the CRU, and the IDB loader
 * (its Rockusb mode included) expects the 24 MHz state it set up itself. Same values as
 * RECHO's switch to 24 MHz (0x030516c4); inter_con0 as RECHO sets it at or below 180 MHz. */
static void clock_slow(void)
{
    REG(CRU_MODE_CON) = 0x00010000;
    spin(2000);
    REG(CRU_CLKSEL0) = 0xffff0000;
    spin(2000);
    REG(CRU_CLKSEL1) = 0x03000000;
    spin(2000);
    REG(GRF_INTER_CON0) = 0xf0400000;           /* multi-cycle off, core B RAMs included */
}

void reboot(void)
{
    __asm volatile ("cpsid i");
    REG(SYST_CSR) = 0;
    clock_slow();
    __asm volatile ("dsb");
    REG(SCB_AIRCR) = 0x05fa0004;                /* SYSRESETREQ */
    for (;;)
        ;
}

/* The IDB loader sees the tag at boot and enters its Rockusb mode (rkdeveloptool) */
void reboot_loader_usb(void)
{
    REG(PMU_SYS_REG3) = 0xffff0000u | BOOTTAG_LOADER_USB;
    reboot();
}

void power_off(void)
{
    gpio_set(PIN_BACKLIGHT, 0);
    gpio_set(PIN_POWER_HOLD, 0);
    mdelay(500);
    /* still powered: the USB cable is in */
}

/* The compiler may emit calls to these for structure and array initialisers */
void *memset(void *d, int c, unsigned n)
{
    uint8_t *p = d;
    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

int memcmp(const void *a, const void *b, unsigned n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *memcpy(void *d, const void *s, unsigned n)
{
    uint8_t *p = d;
    const uint8_t *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}
