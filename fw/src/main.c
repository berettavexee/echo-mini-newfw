/* Step 3, first image: boot, power, clocks, panel, keys and ADC on a test screen, and a
 * firmware installer reading *.IMG files from the root of the internal volume.
 * No sound, no USB. The only flash writes are the installer's, inside the firmware area. */
#include <stdint.h>
#include "audio.h"
#include "board.h"
#include "emmc.h"
#include "fat.h"
#include "font.h"
#include "i2c.h"
#include "lcd.h"
#include "log.h"
#include "msc.h"
#include "player.h"
#include "hifi.h"
#include "regs.h"
#include "rtc.h"
#include "sd.h"
#include "update.h"
#include "usb.h"
#include "version.h"
#include "sys.h"
#include "keys.h"
#ifdef ECHO_PROBE
#include "probe.h"
#endif
#include "ui.h"

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
static void line(int y, const char *s, uint16_t fg)
{
    int x = lcd_text(4, y, s, fg, BG);
    lcd_fill(x, y, LCD_W - x, 16, BG);
}

/* Status lines are redrawn only when their text or colour changed: every pixel goes through
 * the VOP register, so a full line costs several milliseconds */
#define STATUS_LINES 5
static char status_text[STATUS_LINES][64];
static uint16_t status_fg[STATUS_LINES];

static void status_line(int i, int y, const char *s, uint16_t fg)
{
    int n = 0;
    while (s[n] && s[n] == status_text[i][n])
        n++;
    if (!s[n] && !status_text[i][n] && fg == status_fg[i])
        return;
    for (n = 0; (status_text[i][n] = s[n]); n++)
        ;
    status_fg[i] = fg;
    line(y, s, fg);
}

static void status_reset(void)
{
    for (int i = 0; i < STATUS_LINES; i++)
        status_text[i][0] = (char)0xff;
}

static int fast, storage_err = 1, volume_err = 1, volume_dirty;

static void volume_init(void)
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
    volume_init();
#ifdef SD_WALK
    sd_test();
#endif
}

static void draw_static(void)
{
    char b[64], *p;

    status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    line(0, FW_NAME " " FW_VERSION " (" FW_DATE ")", TITLE);
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
    line(16, b, fast && cpu_hz == 200000000 ? FG : WARN);
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
    line(32, b, storage_err || volume_err || volume_dirty ? WARN : FG);
    line(154, "any key: back", DIM);
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
    status_line(0, 48, b, FG);
    p = put_str(b, "battery ");
    p = put_dec(p, mv);
    p = put_str(p, " mV  usb ");
    p = put_str(p, vbus() ? "yes" : "no");
    p = put_str(p, "  chg ");
    p = put_dec(p, gpio_get(PIN_CHARGING));
    *p = 0;
    status_line(1, 64, b, mv && mv < 3400 ? WARN : FG);
    p = put_str(b, "jack 3.5 ");
    p = put_str(p, gpio_get(PIN_JACK35) ? "in" : "-");
    p = put_str(p, "  4.4 ");
    p = put_str(p, gpio_get(PIN_JACK44) ? "-" : "in");
    p = put_str(p, "  adc2 ");
    p = put_dec(p, adc_read(2));
    p = put_str(p, " adc3 ");
    p = put_dec(p, adc_read(3));
    *p = 0;
    status_line(2, 80, b, FG);
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
    status_line(3, 96, b, logs.attached ? DIM : WARN);
}

static void message(int y, const char *s, uint16_t fg)
{
    line(y, s, fg);
}

/* ---- firmware menu ---- */
#define MAX_IMG 32

struct img_list {
    struct fat_file f[MAX_IMG];
    int n;
};

static int is_img(const char *name)
{
    int n = 0;
    while (name[n])
        n++;
    return n > 4 && name[n - 4] == '.' && name[n - 3] == 'I' && name[n - 2] == 'M' && name[n - 1] == 'G';
}

static int collect(const struct fat_file *f, void *ctx)
{
    struct img_list *l = ctx;
    if (is_img(f->name) && f->size >= 0x200)
        l->f[l->n++] = *f;
    return l->n == MAX_IMG;
}

static void progress(int stage, uint32_t done, uint32_t total)
{
    static const char *const what[] = { "checking CRC", "writing FW2", "writing FW1" };
    char b[48], *p = put_str(b, what[stage]);
    p = put_str(p, " ");
    p = put_dec(p, (uint32_t)((uint64_t)done * 100 / total));
    p = put_str(p, " %");
    *p = 0;
    message(112, b, FG);
    int w = (int)((uint64_t)done * (LCD_W - 8) / total);
    lcd_fill(4, 130, w, 6, stage ? WARN : TITLE);
}

static char *put_date(char *p, const struct fw_info *in)
{
    p = put_hex(p, in->year, 4);
    *p++ = '-';
    p = put_hex(p, in->month, 2);
    *p++ = '-';
    return put_hex(p, in->day, 2);
}

/* Returns when the user goes back; reboots after a successful install */
static void firmware_menu(void)
{
    static struct img_list l;
    struct fw_info info;
    char b[64], *p;
    int sel = 0;

    l.n = 0;
    if (!storage_err && !volume_err)
        fat_list_root(collect, &l);
    for (;;) {
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        line(0, "Install firmware (*.IMG, internal root)", TITLE);
        if (storage_err || volume_err)
            line(32, "internal volume not available", WARN);
        else if (!l.n)
            line(32, "no .IMG file in the root folder", WARN);
        for (int i = 0; i < l.n; i++) {
            p = put_str(b, i == sel ? "> " : "  ");
            p = put_str(p, l.f[i].name);
            p = put_str(p, "  ");
            p = put_dec(p, l.f[i].size >> 10);
            p = put_str(p, " KiB");
            *p = 0;
            line(18 + 14 * i, b, i == sel ? FG : DIM);
        }
        line(154, "VOL+/VOL-: choose  PLAY: check  BACK", DIM);

        int k;
        while (!(k = key_edge()))
            wdt_kick();
        if (k == KEY_BACK)
            return;
        if (k == KEY_VOLUP && l.n)              /* the list wraps around */
            sel = sel > 0 ? sel - 1 : l.n - 1;
        if (k == KEY_VOLDOWN && l.n)
            sel = sel < l.n - 1 ? sel + 1 : 0;
        if (k != KEY_PLAY || !l.n)
            continue;

        lcd_fill(0, 18, LCD_W, 152, BG);
        p = put_str(b, l.f[sel].name);
        *p = 0;
        line(18, b, FG);
        int r = fw_check(&l.f[sel], &info, progress);
        log_printf("install: %s, %u bytes: check %d (%s)", l.f[sel].name, l.f[sel].size, r,
                   r ? upd_error(r) : "valid");
        if (r) {
            message(112, upd_error(r), WARN);
            line(154, "BACK: return", DIM);
            while (key_edge() != KEY_BACK)
                wdt_kick();
            continue;
        }
        p = put_str(b, "valid image, ");
        p = put_date(p, &info);
        p = put_str(p, ", ");
        p = put_dec(p, info.fw_size >> 20);
        p = put_str(p, " MiB");
        *p = 0;
        line(36, b, FG);
        line(54, info.two_copies ? "writes FW2, then FW1" : "too big for FW2: writes FW1 only", FG);
        unsigned mv = battery_mv();
        if (!vbus() && mv < 3600) {
            message(80, "battery too low: plug the USB cable", WARN);
            line(154, "BACK: return", DIM);
            while (key_edge() != KEY_BACK)
                wdt_kick();
            continue;
        }
        line(80, "do not power off during the install", WARN);
        line(154, "PLAY: install   BACK: cancel", DIM);
        while ((k = key_edge()) != KEY_PLAY && k != KEY_BACK)
            wdt_kick();
        if (k == KEY_BACK)
            continue;
        line(154, "", DIM);
        log_printf("install: writing %s: fw_end %x, %u bytes, %s", l.f[sel].name, info.fw_end,
                   info.fw_size, info.two_copies ? "FW2 then FW1" : "FW1 only");
        log_flush();
        uint32_t t0 = millis();
        r = fw_install(&l.f[sel], &info, progress);
        log_printf("install: result %d (%s) in %u ms", r, r ? upd_error(r) : "ok", millis() - t0);
        log_flush();
        if (r) {
            message(112, upd_error(r), WARN);
            message(130, "restart, then install again", WARN);
            line(154, "BACK: return", DIM);
            while (key_edge() != KEY_BACK)
                wdt_kick();
            continue;
        }
        message(112, "installed: restarting", TITLE);
        mdelay(1500);
        clock_fast_ok();                        /* this loop skips sys_idle(): a restart on */
        reboot();                               /* purpose is not a failed 200 MHz attempt */
    }
}

/* ---- USB disk: the user volume, read only ---- */
static void usb_screen(void)
{
    char b[64], *p;

    p = put_str(b, usb.configured ? "configured, " : "waiting for the host, ");
    p = put_str(p, usb.high_speed ? "high speed" : "full speed");
    *p = 0;
    status_line(0, 36, b, usb.configured ? FG : DIM);
    p = put_str(b, "cmds ");
    p = put_dec(p, msc.commands);
    p = put_str(p, "  read ");
    p = put_dec(p, msc.read_sectors / 2048);
    p = put_str(p, "  written ");
    p = put_dec(p, msc.written_sectors / 2048);
    p = put_str(p, " MiB");
    *p = 0;
    status_line(1, 52, b, FG);
    p = put_str(b, "op ");
    p = put_hex(p, msc.last_op, 2);
    p = put_str(p, "  errors ");
    p = put_dec(p, msc.errors);
    p = put_str(p, "  resets ");
    p = put_dec(p, usb.resets);
    p = put_str(p, "  setups ");
    p = put_dec(p, usb.setups);
    *p = 0;
    status_line(2, 68, b, msc.errors ? WARN : DIM);
    p = put_str(b, "disk @");
    p = put_hex(p, msc.base, 6);
    p = put_str(p, " ");
    p = put_dec(p, msc.count / 2048);
    p = put_str(p, " MiB");
    *p = 0;
    status_line(3, 84, b, DIM);
}

/* writable: the host may write the user volume (and only it: emmc_write_user's fence) */
static void usb_mode(int writable)
{
    uint32_t total = 0;

    status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    line(0, writable ? "USB disk (read/write)" : "USB disk (read only)", writable ? WARN : TITLE);
    if (storage_err || volume_err || emmc_sectors(&total) || total <= vol.base) {
        line(36, "internal volume not available", WARN);
        mdelay(2000);
        return;
    }
    if (!vbus()) {
        line(36, "plug the USB cable", WARN);
        mdelay(2000);
        return;
    }
    line(154, "BACK: exit (eject on the computer first)", DIM);
    if (writable)
        line(122, "eject on the computer before unplugging", WARN);
    if (msc_start(vol.base, total - vol.base, writable)) {
        line(36, "write fence refused: volume inside the system area", WARN);
        log_printf("usb: write fence refused for %x + %u", vol.base, total - vol.base);
        mdelay(3000);
        return;
    }
    log_printf("usb: %s disk, sectors %x + %u", writable ? "read/write" : "read only", vol.base,
               total - vol.base);
#ifdef ECHO_PROBE
    probe_attach(vol.base);                     /* before the host owns the volume */
#endif
    log_detach();                               /* the host owns the volume from here */
    usb_start();
    uint32_t t_draw = 0, t_key = 0, t_eject = 0, t0 = millis(), configured_at = 0;
    const char *why = "?";
    for (;;) {
        wdt_kick();
        msc_poll();
        uint32_t now = ticks();
        if (usb.configured && !configured_at) {
            configured_at = millis();
            log_printf("usb: configured after %u ms, %s speed", configured_at - t0,
                       usb.high_speed ? "high" : "full");
        }
        if (now - t_key > 10000) {
            t_key = now;
            if (!vbus()) {
                why = "cable unplugged";
                break;
            }
            if (key_edge() == KEY_BACK) {
                why = "BACK";
                break;
            }
        }
        if (now - t_draw > 250000) {
            usb_screen();
            t_draw = now;
        }
        /* The host stopped the bus (udisksctl power-off, port disabled): a mounted volume is
         * polled every few seconds, so 5 s without activity means it is gone */
        if (usb.suspended && now - usb.suspend_since > 5000000) {
            why = "bus suspended 5 s";
            break;
        }
        if (msc.ejected) {
            if (!t_eject)
                t_eject = now;
            else if (now - t_eject > 500000) {
                why = "ejected";
                break;
            }
        }
    }
    usb_stop();
    log_printf("usb: exit (%s) after %u s: %u commands, read %u written %u sectors, %u errors, "
               "%u resets, %u setups", why, (millis() - t0) / 1000, msc.commands, msc.read_sectors,
               msc.written_sectors, msc.errors, usb.resets, usb.setups);
    volume_init();                              /* the host may have changed everything */
#ifdef ECHO_PROBE
    probe_after_usb();
#endif
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
            message(122, "power off", WARN);
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

static int newest_write(const struct fat_dirent *e, void *ctx)
{
    uint32_t *m = ctx;
    if (e->mtime > *m)
        *m = e->mtime;
    return 0;
}

/* After a read/write session the clock may be set from what the computer wrote (rtc_from_fat) */
void sys_usb(int writable)
{
    usb_mode(writable);
    uint32_t newest = 0;
    if (writable && !volume_err && !fat_list_dir(0, newest_write, &newest) && newest)
        rtc_from_fat(newest);
}

void sys_install(void)
{
    firmware_menu();
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
    message(122, "USB loader mode (exit: RST)", WARN);
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
