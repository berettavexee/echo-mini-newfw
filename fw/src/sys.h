/* What main.c offers to the user interface (ui.c): colours, number formatting, housekeeping,
 * and the system functions of the main menu */
#ifndef SYS_H
#define SYS_H

#include <stdint.h>
#include "lcd.h"

#define FG    RGB(230, 230, 230)
#define DIM   RGB(120, 120, 120)
#define BG    RGB(0, 0, 0)
#define TITLE RGB(255, 200, 0)
#define WARN  RGB(255, 80, 60)

#ifndef SCREEN_TIMEOUT_US
#define SCREEN_TIMEOUT_US 30000000u     /* screen off after 30 s without a key */
#endif

char *put_str(char *p, const char *s);
char *put_dec(char *p, uint32_t v);
char *put_hex(char *p, uint32_t v, int digits);

void sys_idle(void);            /* watchdog, log, battery, clock marker, Power key */
void cpu_wait(void);            /* sleep until the next interrupt (SysTick: 1 ms at most) */
int sys_screen_is_off(void);    /* switched off by a short press of Power, or after 30 s */
void sys_screen_set(int on);    /* backlight and panel sleep together */
void sys_power_off(const char *why);    /* saves the state, powers off (returns on USB power) */
void sys_usb(int writable);     /* USB mass storage until ejected */
void sys_install(void);         /* firmware installer */
void sys_screen(void);          /* the diagnostic screen, until a key */
void sys_loader_usb(void);      /* reboot into the loader's USB mode */

/* Charging screen (charge.c): at boot, when the USB cable switched the player on (returns at
 * once otherwise, or for the normal start), or after Power held with the cable in (never
 * returns: restart or off) */
void charge_screen(int at_boot);
extern int sys_power_mine;      /* the charging screen reads Power itself (not sys_idle) */

#endif
