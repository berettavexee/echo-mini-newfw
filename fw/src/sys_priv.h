/* What main.c shares with the other system screens (install.c, usbmode.c); the user interface
 * uses sys.h */
#ifndef SYS_PRIV_H
#define SYS_PRIV_H

#include <stdint.h>

extern int storage_err, volume_err;             /* eMMC/IDB, internal volume: 0 = ready */

void sys_line(int y, const char *s, uint16_t fg);   /* a text line padded to the right edge */
/* Status lines are redrawn only when their text or colour changed */
void sys_status_line(int i, int y, const char *s, uint16_t fg);
void sys_status_reset(void);
void sys_volume_init(void);     /* mounts the internal volume, attaches the log */

#endif
