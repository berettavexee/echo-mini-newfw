/* Boot (clocks, storage, internal volume, log), then the user interface (ui.c); the system
 * functions it calls (sys.h): idle housekeeping, power and screen, the test screen; and the
 * fault screen. The firmware installer is in install.c, the USB disk mode in usbmode.c. */
#include <stdint.h>
#include "board.h"
#include "emmc.h"
#include "fat.h"
#include "font.h"
#include "i2c.h"
#include "lcd.h"
#include "log.h"
#include "regs.h"
#include "rtc.h"
#include "sd.h"
#include "version.h"
#include "sys.h"
#include "keys.h"
#include "ui.h"
#include "sys_priv.h"

extern uint32_t boot_regs[4], boot_tag;

char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

char *put_dec(char *p, uint32_t v)
{
    char t[10];
    int n = 0;
    do
        t[n++] = '0' + v % 10;
    while (v /= 10);
    while (n)
        *p++ = t[--n];
    return p;
}

char *put_hex(char *p, uint32_t v, int digits)
{
    while (digits--)
        *p++ = "0123456789abcdef"[(v >> (4 * digits)) & 15];
    return p;
}

/* One text line, padded with the background up to the right edge */
void sys_line(int y, const char *s, uint16_t fg)
{
    int x = lcd_text(4, y, s, fg, BG);
    lcd_fill(x, y, LCD_W - x, 16, BG);
}

/* Status lines are redrawn only when their text or colour changed: every pixel goes through
 * the VOP register, so a full line costs several milliseconds */
#define STATUS_LINES 5
static char status_text[STATUS_LINES][64];
static uint16_t status_fg[STATUS_LINES];

void sys_status_line(int i, int y, const char *s, uint16_t fg)
{
    int n = 0;
    while (s[n] && s[n] == status_text[i][n])
        n++;
    if (!s[n] && !status_text[i][n] && fg == status_fg[i])
        return;
    for (n = 0; (status_text[i][n] = s[n]); n++)
        ;
    status_fg[i] = fg;
    sys_line(y, s, fg);
}

void sys_status_reset(void)
{
    for (int i = 0; i < STATUS_LINES; i++)
        status_text[i][0] = (char)0xff;
}

static int fast, volume_dirty;
int storage_err = 1, volume_err = 1;

void sys_volume_init(void)
{
    volume_err = fat_mount(EMMC_FW1 + emmc.p);
    volume_dirty = volume_err ? 0 : fat_dirty();
    if (volume_err) {
        log_printf("volume: no FAT32 volume found (%d)", volume_err);
        return;
    }
    log_printf("volume: MBR/boot at %x, boot sector %x, FAT %x, data %x, %u sectors/cluster, "
               "%u clusters, state %s (%d)", vol.base, vol.part, vol.fat, vol.data, vol.spc,
               vol.clusters, volume_dirty ? "dirty" : "clean", volume_dirty);
    int r = log_attach();
    static const char *const why[] = { "", "no " LOG_FILE " in the root", "file fragmented",
                                       "file size outside 8 KiB..16 MiB", "outside the volume",
                                       "read error" };
    if (r)
        log_printf("log: not written to the volume: %s", why[-r < 6 ? -r : 0]);
    else
        log_printf("log: %s, %u sectors, next %u, wraps %u", LOG_FILE, logs.file_sectors,
                   logs.next, logs.wraps);
}

static void storage_init(void)
{
    uint32_t total = 0;

    storage_err = emmc_init();
    if (storage_err) {
        log_printf("emmc: IDB not found (%d)", storage_err);
        return;
    }
    int r = emmc_sectors(&total);
    log_printf("emmc: %u sectors (%d); IDB copy %d: P %x R %x", total, r, emmc.idb_copy, emmc.p,
               emmc.r);
    sys_volume_init();
#ifdef SD_WALK
    sd_test();
#endif
}

static void draw_static(void)
{
    char b[64], *p;

    sys_status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    sys_line(0, FW_NAME " " FW_VERSION " (" FW_DATE ")", TITLE);
    p = put_str(b, "CPU ");
    p = put_dec(p, cpu_hz / 1000000);
    p = put_str(p, fast ? " MHz" : " MHz (fallback)");
    if (fast) {
        p = put_str(p, " adc ");
        p = put_dec(p, adc_cycles_24m);
        p = put_str(p, "/");
        p = put_dec(p, adc_cycles);
        p = put_str(p, " pll ");
        p = put_hex(p, pll_con1 & 0xffff, 4);
    }
    *p = 0;
    sys_line(16, b, fast && cpu_hz == 200000000 ? FG : WARN);
    if (storage_err) {
        p = put_str(b, "eMMC/IDB error ");
        p = put_dec(p, -storage_err);
    } else {
        p = put_str(b, "IDB P ");
        p = put_hex(p, emmc.p, 5);
        p = put_str(p, " R ");
        p = put_hex(p, emmc.r, 5);
        if (volume_err)
            p = put_str(p, "  no FAT32 volume");
        else {
            p = put_str(p, "  vol @");
            p = put_hex(p, vol.part, 6);
            p = put_str(p, " ");
            p = put_dec(p, vol.spc / 2);
            p = put_str(p, "K");
            if (volume_dirty < 0)
                p = put_str(p, " ?");
            else if (volume_dirty) {
                p = put_str(p, " dirty ");     /* 1: boot sector (Linux), 2: FAT[1], 4: error */
                p = put_dec(p, (uint32_t)volume_dirty);
            } else
                p = put_str(p, " clean");
        }
    }
    *p = 0;
    sys_line(32, b, storage_err || volume_err || volume_dirty ? WARN : FG);
    sys_line(154, "any key: back", DIM);
}

static uint32_t draw_us;                        /* last status refresh that drew something */

static void draw_status(void)
{
    char b[64], *p;
    unsigned k = adc_read(1), mv = battery_mv();

    p = put_str(b, "key ");
    p = put_str(p, key_name(key_ladder(k)));
    p = put_str(p, " (adc ");
    p = put_dec(p, k);
    p = put_str(p, ")  power ");
    p = put_str(p, power_key() ? "ON" : "-");
    *p = 0;
    sys_status_line(0, 48, b, FG);
    p = put_str(b, "battery ");
    p = put_dec(p, mv);
    p = put_str(p, " mV  usb ");
    p = put_str(p, vbus() ? "yes" : "no");
    p = put_str(p, "  chg ");
    p = put_dec(p, gpio_get(PIN_CHARGING));
    *p = 0;
    sys_status_line(1, 64, b, mv && mv < 3400 ? WARN : FG);
    p = put_str(b, "jack 3.5 ");
    p = put_str(p, gpio_get(PIN_JACK35) ? "in" : "-");
    p = put_str(p, "  4.4 ");
    p = put_str(p, gpio_get(PIN_JACK44) ? "-" : "in");
    p = put_str(p, "  adc2 ");
    p = put_dec(p, adc_read(2));
    p = put_str(p, " adc3 ");
    p = put_dec(p, adc_read(3));
    *p = 0;
    sys_status_line(2, 80, b, FG);
    uint32_t s = ticks() / 1000000;
    p = put_str(b, "up ");
    p = put_dec(p, s / 60);
    *p++ = ':';
    *p++ = '0' + s % 60 / 10;
    *p++ = '0' + s % 10;
    p = put_str(p, "   draw ");
    p = put_dec(p, draw_us / 1000);
    p = put_str(p, " ms   log ");
    if (logs.attached)
        p = put_str(p, "on");
    else {
        p = put_str(p, "off ");
        p = put_dec(p, (uint32_t)-logs.error);
    }
    *p = 0;
    sys_status_line(3, 96, b, logs.attached ? DIM : WARN);
}

/* ---- for the user interface (sys.h) ---- */
static uint32_t sys_t_start, sys_t_bat, sys_t_power;
static int sys_fast_cleared, sys_power_released, sys_screen_off;
static uint32_t a_idle;                         /* TIMER1 ticks asleep in cpu_wait() */

void sys_idle(void)
{
    wdt_kick();
    uint32_t now = ticks();
    if (!sys_fast_cleared && now - sys_t_start > 5000000) {
        /* 5 s without trouble: at 200 MHz, the attempt is good; in the 24 MHz fallback, the
         * marker must go too, or every later boot would stay in the fallback (0.23) */
        clock_fast_ok();
        sys_fast_cleared = 1;
    }
    log_poll();
    rtc_service();
    if (millis() - sys_t_bat > 60000) {
        static uint32_t t_win, idle_win;
        struct rtc_tm t;
        int c = rtc_get(&t);
        uint32_t now24 = timer24(), el = now24 - t_win, sl = a_idle - idle_win;
        t_win = now24;
        idle_win = a_idle;
        sys_t_bat = millis();
        log_printf("battery %u mV, usb %d, charge pin %d, font reads %u, clock %02u:%02u:%02u%s, "
                   "core A busy %u.%u %%", battery_mv(), vbus(), gpio_get(PIN_CHARGING),
                   font_misses(), c < 0 ? 0 : t.hour, c < 0 ? 0 : t.min, c < 0 ? 0 : t.sec,
                   c == 1 ? "" : c ? " (not set)" : " (none)",
                   el > sl ? (uint32_t)((uint64_t)(el - sl) * 1000 / el) / 10 : 0,
                   el > sl ? (uint32_t)((uint64_t)(el - sl) * 1000 / el) % 10 : 0);
    }
    /* Power: a short press switches the screen off and on, 2 s switch the player off (with the
     * cable in: the charging screen) */
    if (sys_power_mine) {
    } else if (!sys_power_released) {
        sys_power_released = !power_key();      /* ignore the press that switched us on */
    } else if (power_key()) {
        if (!sys_t_power)
            sys_t_power = now ? now : 1;
        else if (now - sys_t_power > 2000000) {
            sys_screen_set(1);
            sys_line(122, "power off", WARN);
            sys_power_off("Power 2 s");
            charge_screen(0);                   /* still powered: the USB cable is in */
        }
    } else if (sys_t_power) {
        if (now - sys_t_power > 30000)          /* a press, not a bounce */
            sys_screen_set(sys_screen_off);
        sys_t_power = 0;
    }
}

/* The state saved, then the power hold released; returns when the USB cable still powers us */
void sys_power_off(const char *why)
{
    ui_power_off();
    log_printf("power off (%s), battery %u mV", why, battery_mv());
    log_flush();
    mdelay(300);
    power_off();
    log_printf("power off: still powered (USB)");
}

/* WFI only while the 1 ms SysTick interrupt runs, so that the wait is bounded. A function of
 * its own: the emulator (tools/emu_fw.py), which has no SysTick, replaces it by a return. */
__attribute__((noinline)) void cpu_wait(void)
{
    if (REG(SYST_CSR) & 2) {
        uint32_t t0 = timer24();
        __asm volatile ("wfi");
        a_idle += timer24() - t0;
    }
}

int sys_screen_is_off(void)
{
    return sys_screen_off;
}

void sys_screen_set(int on)
{
    if (on == !sys_screen_off)
        return;
    sys_screen_off = !on;
    if (on) {
        if (clock_is_low)
            clock_low(0);                       /* the screen at full speed (ui.c power_service) */
        lcd_sleep(0);
        lcd_backlight(1);
    } else {
        lcd_backlight(0);
        lcd_sleep(1);
    }
    log_printf("screen %s", on ? "on" : "off");
}

void sys_screen(void)
{
    draw_static();
    draw_status();
    key_edge_sync();
    for (uint32_t t_draw = 0;;) {
        sys_idle();
        if (ticks() - t_draw > 50000) {
            draw_status();
            t_draw = ticks();
        }
        if (key_edge())
            return;
    }
}

void sys_loader_usb(void)
{
    sys_line(122, "USB loader mode (exit: RST)", WARN);
    log_printf("reboot to the loader USB mode (main menu)");
    log_flush();
    mdelay(1000);
    reboot_loader_usb();
}

/* An empty battery at start-up (ETUDE §43): below 3,350 mV without USB power (the mean of 8
 * readings), a message and power off, before any resume or write but this line of the log */
static void battery_check_boot(void)
{
    if (vbus())
        return;
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) {
        sum += battery_mv();
        mdelay(5);
    }
    if (sum / 8 >= 3350)
        return;
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    lcd_text_clip_x(8, 50, "Battery empty", LCD_W, WARN, BG, 2);
    lcd_text(8, 100, "plug the USB cable", DIM, BG);
    mdelay(2000);
    log_printf("power off (battery empty at start), battery %u mV", sum / 8);
    log_flush();                                /* not sys_power_off(): nothing loaded yet, */
    mdelay(300);                                /* the saved resume state must stay as it is */
    power_off();
    log_printf("power off: still powered (USB)");
}

int main(void)
{
    gpio_set(PIN_POWER_HOLD, 1);                /* the loader set it already; keep it */
    gpio_dir(PIN_POWER_HOLD, 1);
    iomux(PIN_POWER_KEY, 0);
    gpio_dir(PIN_POWER_KEY, 0);
    gpio_pull(PIN_JACK35, 0);                   /* externally driven: RECHO turns the pull off */
    gpio_dir(PIN_JACK35, 0);
    gpio_dir(2, 8, 0);
    gpio_dir(2, 3, 0);

    fast = clock_init();
    timer_init();
    timer24_init();
    log_init();
    log_printf("=== boot %u: " FW_NAME " " FW_VERSION " (" FW_DATE "), tag %x, r0 %x r2 %x",
               logs.boots, boot_tag & 0xffff, boot_regs[0], boot_regs[2]);
    if (logs.recovered)
        log_printf("log: %u bytes of the previous boot recovered from PMU-SRAM", logs.recovered);
    log_printf("clock: %u MHz%s, mode %x sel0 %x sel1 %x pll %x, tries %u, adc %u/%u",
               cpu_hz / 1000000, fast ? "" : " (fallback: 24 MHz)", cru_mode, cru_sel0, cru_sel1,
               pll_con1_after & 0xffff, mode_tries, adc_cycles_24m, adc_cycles);
    {
        uint32_t t0 = timer24(), u0 = ticks();
        udelay(1000);
        log_printf("timer: TIMER1 %u ticks in %u us (24 expected per us)", timer24() - t0, ticks() - u0);
    }
    lcd_init();
    lcd_text(4, 0, "starting...", DIM, BG);
    lcd_backlight(1);
    storage_init();
    font_init();
    rtc_init();
    log_printf("battery %u mV, usb %d, charge pin %d, jack 3.5 %d 4.4 %d, power key %d",
               battery_mv(), vbus(), gpio_get(PIN_CHARGING), gpio_get(PIN_JACK35),
               !gpio_get(PIN_JACK44), power_key());
    log_flush();
    battery_check_boot();
    charge_screen(1);                           /* switched on by the USB cable: until PLAY held */
    draw_static();
    draw_status();

    sys_t_start = ticks();
    sys_t_bat = millis();
    ui_main();
}

/* Called by the fault handlers: exception number and stacked frame (r0-r3, r12, lr, pc, psr) */
void fault_screen(const uint32_t *frame, uint32_t exc)
{
    char b[64], *p;

    __asm volatile ("cpsid i");
    lcd_wake_fault();
    /* into the PMU-SRAM ring only: the next boot writes it to the file */
    log_printf("FAULT exc %u pc %x lr %x psr %x cfsr %x hfsr %x bfar %x", exc,
               frame ? frame[6] : 0, frame ? frame[5] : 0, frame ? frame[7] : 0,
               REG(0xe000ed28), REG(0xe000ed2c), REG(0xe000ed38));
    if (!lcd_ready) {
        power_off();
        reboot();
    }
    lcd_fill(0, 0, LCD_W, LCD_H, RGB(128, 0, 0));
    p = put_str(b, "FAULT exc ");
    p = put_dec(p, exc);
    p = put_str(p, " cfsr ");
    p = put_hex(p, REG(0xe000ed28), 8);
    *p = 0;
    lcd_text(4, 4, b, FG, RGB(128, 0, 0));
    if (frame) {
        p = put_str(b, "pc ");
        p = put_hex(p, frame[6], 8);
        p = put_str(p, " lr ");
        p = put_hex(p, frame[5], 8);
        *p = 0;
        lcd_text(4, 24, b, FG, RGB(128, 0, 0));
    }
    p = put_str(b, "bfar ");
    p = put_hex(p, REG(0xe000ed38), 8);
    *p = 0;
    lcd_text(4, 44, b, FG, RGB(128, 0, 0));
    lcd_text(4, 84, "any key: restart   Power 2 s: off", FG, RGB(128, 0, 0));
    lcd_backlight(1);
    /* no SysTick here: busy-wait on the ADC (about 20 us per read) */
    for (unsigned n = 0;;) {
        wdt_kick();
        if (power_key()) {
            if (++n > 100000)
                power_off();
        } else if (n) {
            reboot();
        } else if (key_read())
            reboot();
    }
}
