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

char *put_str(char *p, const char *s);
char *put_dec(char *p, uint32_t v);
char *put_hex(char *p, uint32_t v, int digits);

int key_scan(void);             /* debounced ladder key (30 ms; held: see main.c) */
const char *key_name(int k);
extern unsigned key_adc;        /* channel 1 when the key held was taken */
void sys_idle(void);            /* watchdog, log, battery, clock marker, Power key */
void cpu_wait(void);            /* sleep until the next interrupt (SysTick: 1 ms at most) */
int sys_screen_is_off(void);    /* switched off by a short press of Power, or after 30 s */
void sys_screen_set(int on);    /* backlight and panel sleep together */
void sys_power_off(const char *why);    /* saves the state, powers off (returns on USB power) */
void sys_usb(int writable);     /* USB mass storage until ejected */
void sys_install(void);         /* firmware installer */
void sys_tests(void);           /* audio test menu */
void sys_screen(void);          /* the diagnostic screen, until a key */
void sys_loader_usb(void);      /* reboot into the loader's USB mode */

#endif
