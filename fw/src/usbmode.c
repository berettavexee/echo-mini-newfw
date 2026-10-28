/* USB disk mode: the user volume for the computer (msc.c over usb.c), and its status screen */
#include <stdint.h>
#include "board.h"
#include "emmc.h"
#include "fat.h"
#include "keys.h"
#include "log.h"
#include "msc.h"
#include "rtc.h"
#include "sys.h"
#include "sys_priv.h"
#include "usb.h"
#ifdef ECHO_PROBE
#include "probe.h"
#endif

/* The user volume: read only, or read/write */
static void usb_screen(void)
{
    char b[64], *p;

    p = put_str(b, usb.configured ? "configured, " : "waiting for the host, ");
    p = put_str(p, usb.high_speed ? "high speed" : "full speed");
    *p = 0;
    sys_status_line(0, 36, b, usb.configured ? FG : DIM);
    p = put_str(b, "cmds ");
    p = put_dec(p, msc.commands);
    p = put_str(p, "  read ");
    p = put_dec(p, msc.read_sectors / 2048);
    p = put_str(p, "  written ");
    p = put_dec(p, msc.written_sectors / 2048);
    p = put_str(p, " MiB");
    *p = 0;
    sys_status_line(1, 52, b, FG);
    p = put_str(b, "op ");
    p = put_hex(p, msc.last_op, 2);
    p = put_str(p, "  errors ");
    p = put_dec(p, msc.errors);
    p = put_str(p, "  resets ");
    p = put_dec(p, usb.resets);
    p = put_str(p, "  setups ");
    p = put_dec(p, usb.setups);
    *p = 0;
    sys_status_line(2, 68, b, msc.errors ? WARN : DIM);
    p = put_str(b, "disk @");
    p = put_hex(p, msc.base, 6);
    p = put_str(p, " ");
    p = put_dec(p, msc.count / 2048);
    p = put_str(p, " MiB");
    *p = 0;
    sys_status_line(3, 84, b, DIM);
}

/* writable: the host may write the user volume (and only it: emmc_write_user's fence) */
static void usb_mode(int writable)
{
    uint32_t total = 0;

    sys_status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    sys_line(0, writable ? "USB disk (read/write)" : "USB disk (read only)", writable ? WARN : TITLE);
    if (storage_err || volume_err || emmc_sectors(&total) || total <= vol.base) {
        sys_line(36, "internal volume not available", WARN);
        mdelay(2000);
        return;
    }
    if (!vbus()) {
        sys_line(36, "plug the USB cable", WARN);
        mdelay(2000);
        return;
    }
    sys_line(154, "BACK: exit (eject on the computer first)", DIM);
    if (writable)
        sys_line(122, "eject on the computer before unplugging", WARN);
    if (msc_start(vol.base, total - vol.base, writable)) {
        sys_line(36, "write fence refused: volume inside the system area", WARN);
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
    sys_volume_init();                              /* the host may have changed everything */
#ifdef ECHO_PROBE
    probe_after_usb();
#endif
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
