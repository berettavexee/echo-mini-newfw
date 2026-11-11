/* The 96 KiB at 0x03078000 (link.ld, the OVERLAY over .dirbuf) for users that are never active
 * at the same time: the directory listing of the browser (ui.c); in a USB session, the USB
 * overlay's code (ovl.h, its first 16 KiB) and the USB transfer buffer after it (msc.c, 64 KiB);
 * in an installation, the installer's buffers (update.c, 2 x 8 KiB). The main menu stops
 * playback before USB or an installation and reads the folder again afterwards (ETUDE §25).
 * The addresses come from link.ld (symbols, so that tools/ovlcheck.py sees them): until 0.103o1
 * the transfer buffer was at 0x03078000 too, over the USB overlay's code (ETUDE §137). */
#ifndef SHARED_H
#define SHARED_H

#include <stdint.h>

extern uint32_t __shared_usb[], __shared_upd[];
#define SHARED_USB     __shared_usb                         /* 64 KiB */
#define SHARED_UPD_BUF __shared_upd                         /* 8 KiB */
#define SHARED_UPD_CHK (__shared_upd + 0x2000 / 4)          /* 8 KiB */

#endif
