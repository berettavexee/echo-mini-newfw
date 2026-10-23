/* One 96 KiB NOLOAD area at 0x03078000 (link.ld .dirbuf) for three users that are never active
 * at the same time: the directory listing of the browser (ui.c), the USB transfer buffer (msc.c,
 * 64 KiB at the start) and the installer's buffers (update.c, 2 x 8 KiB after it). The main
 * menu stops playback before USB or an installation and reads the folder again afterwards
 * (ETUDE §25). */
#ifndef SHARED_H
#define SHARED_H

#define SHARED_BUF     0x03078000u
#define SHARED_USB     ((uint32_t *)SHARED_BUF)                 /* 64 KiB */
#define SHARED_UPD_BUF ((uint32_t *)(SHARED_BUF + 0x10000))     /* 8 KiB */
#define SHARED_UPD_CHK ((uint32_t *)(SHARED_BUF + 0x12000))     /* 8 KiB */

#endif
