/* Code overlays (ETUDE §137): modules used only in modes that never run together with what
 * lives in their region. Each is linked at its region's address (link.ld, OVERLAY) and stored in
 * the image (tools/mkimg.py, header +0x1e0: "NOVL", the build's tag, then for each overlay its
 * offset, size, CRC-32 and address). ovl_load() reads one from the eMMC copy whose tag is the one
 * this code was built with (FW1, then FW2) and checks its CRC. The rest of the firmware enters an
 * overlay only through the gates of ovl.c (tools/ovlcheck.py checks it on the ELF).
 * The USB overlay's region is the directory listing (.dirbuf): the listing is read again after a
 * USB session (ui.c), and the overlay counts as gone once its gate returns. */
#ifndef OVL_H
#define OVL_H

enum { OVL_NONE, OVL_USB, OVL_COUNT };

extern int ovl_cur;             /* the overlay loaded (OVL_NONE: none) */
int ovl_load(int id);           /* 0, or -1 (logged): no copy with this build's tag, a bad CRC */

/* Inside the USB overlay (usb.c, msc.c, uac.c, usbmode.c), called through the gates */
void sys_usb_ovl(int writable);
void uac_mode_ovl(void);
void usb_isr_ovl(void);

#endif
