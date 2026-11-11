/* Code overlays: the loader and the gates (ovl.h, ETUDE §137) */
#include <stdint.h>
#include "ovl.h"
#include "board.h"
#include "emmc.h"
#include "i18n.h"
#include "library.h"
#include "log.h"
#include "regs.h"
#include "sys.h"
#include "sys_priv.h"
#include "ui_priv.h"

#define OVL_MAGIC   0x4c564f4eu                 /* "NOVL" */

extern uint8_t __ovl_usb_start[], __ovl_usb_end[];  /* its region (link.ld) */

/* The build's tag: tools/mkimg.py writes here the one it put in the image's overlay table (the
 * placeholder below otherwise); volatile: read from memory, never folded */
const volatile uint32_t ovl_tag __attribute__((used)) = 0x4741544fu;   /* "OTAG" */

int ovl_cur;

static const struct {
    const char *name;
    uint8_t *start, *end;
} ovl[OVL_COUNT] = {
    { "none", 0, 0 },
    { "USB", __ovl_usb_start, __ovl_usb_end },
};

/* overlay id from the image at sector img: 0, or the reason */
static const char *load_from(uint32_t img, int id)
{
    /* the sectors read on the way: the region's last 512 bytes (the region is overwritten
     * anyway; an overlay never reaches them) */
    uint32_t *sec = (uint32_t *)(void *)(ovl[id].end - 512);
    const uint8_t *b = (const uint8_t *)sec;
    if (emmc_read(img, 1, sec) || b[0x1f8] != 'R' || b[0x1ff] != 'W')
        return "no image";
    uint32_t off = sec[0x1e0 / 4], len = sec[0x1e4 / 4];
    if (!len || off % 512 || emmc_read(img + off / 512, 1, sec) || sec[0] != OVL_MAGIC ||
        sec[1] != 1)
        return "no overlay table";
    if (sec[2] != ovl_tag)
        return "another build";
    const uint32_t *t = &sec[4 + 4 * (id - 1)];     /* offset, size, CRC, address */
    uint8_t *dst = ovl[id].start;
    uint32_t o = t[0], n = t[1], crc = t[2];
    if ((uint32_t)id > sec[3] || t[3] != (uint32_t)(uintptr_t)dst)
        return "not this layout";
    if (o % 512 || n > (uint32_t)(ovl[id].end - dst) - 512 || o + n > len)
        return "bad table";
    uint32_t whole = n / 512, lba = img + (off + o) / 512;
    if (whole && emmc_read(lba, whole, dst))
        return "read error";
    if (n % 512) {                              /* the last part: no write past the region */
        if (emmc_read(lba + whole, 1, sec))
            return "read error";
        __builtin_memcpy(dst + whole * 512, sec, n % 512);
    }
    if (~lib_crc(~0u, dst, n) != crc)
        return "CRC mismatch";
    return 0;
}

int ovl_load(int id)
{
    if (ovl_cur == id)
        return 0;
    ovl_cur = OVL_NONE;
    const char *why = "no eMMC";
    for (int k = 0; k < 2 && emmc.p; k++) {
        uint32_t t0 = ticks();
        why = load_from(EMMC_FW1 + (k ? emmc.r : 0), id);
        if (!why) {
            ovl_cur = id;
            log_printf("ovl: %s from FW%d in %u us", ovl[id].name, k + 1, ticks() - t0);
            return 0;
        }
        log_printf("ovl: %s: FW%d: %s", ovl[id].name, k + 1, why);
    }
    return -1;
}

static void failed(void)
{
    sys_line(122, T("This part of the firmware is damaged: install it again"), WARN);
    mdelay(3000);
}

/* ---- the gates ---- */
/* The USB modes: their overlay over the directory listing, gone when they return (the caller
 * reads the listing again, ui.c) */
void sys_usb(int writable)
{
    if (ovl_load(OVL_USB))
        failed();
    else
        sys_usb_ovl(writable);
    ovl_cur = OVL_NONE;
}

void uac_mode(void)
{
    if (ovl_load(OVL_USB))
        failed();
    else
        uac_mode_ovl();
    ovl_cur = OVL_NONE;
}

/* interrupt 5 (the USB controller): only while the USB overlay is in, masked otherwise */
void OTG_IRQHandler(void)
{
    if (ovl_cur == OVL_USB)
        usb_isr_ovl();
    else
        REG(NVIC_ICER) = 1u << 5;
}
