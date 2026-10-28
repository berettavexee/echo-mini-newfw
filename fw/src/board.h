/* Board support: pins, clocks, delays, keys, ADC, power. */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/* Pins (bank, pin) of the ECHO MINI */
#define PIN_POWER_KEY   2, 5    /* input, 1 = pressed */
#define PIN_POWER_HOLD  2, 6    /* output, 1 keeps the player on */
#define PIN_BACKLIGHT   2, 4    /* output (or PWM0 function 1); 1 = full brightness */
#define PIN_LCD_RESET   2, 15
#define PIN_LCD_SEL     2, 9    /* input before panel power-up: panel variant */
#define PIN_JACK35      0, 13   /* 1 = 3.5 mm plug present */
#define PIN_JACK44      2, 8    /* 0 = 4.4 mm plug present */
#define PIN_CHARGING    2, 3

enum { KEY_NONE, KEY_VOLDOWN, KEY_VOLUP, KEY_PLAY, KEY_MENU, KEY_BACK };

void iomux(int bank, int pin, int func);
void gpio_dir(int bank, int pin, int out);
void gpio_pull(int bank, int pin, int on);   /* on = the pad's own pull resistor */
void gpio_set(int bank, int pin, int val);
int gpio_get(int bank, int pin);
void clk_gate(int n, int on);
void soft_reset(int n, int on);

extern uint32_t cpu_hz;
extern uint32_t adc_cycles_24m, adc_cycles, pll_con1;
extern uint32_t loop_24m, loop_pll, cru_mode, cru_sel0, cru_sel1, mode_tries, pll_con1_after;   /* read back after the switch */   /* calibration counts (clock_init) */
int clock_init(void);           /* 1 = running at 200 MHz, 0 = stayed on the 24 MHz crystal */
void clock_fast_ok(void);       /* clears the 200 MHz attempt marker */
void clock_low(int on);         /* 1: 24 MHz crystal, PLL off; 0: back to the PLL */
extern int clock_is_low;
void timer_init(void);
uint32_t ticks(void);           /* free-running microsecond counter */
/* TIMER1 (0x400e0020), 64-bit, 24 MHz crystal, free running from 2^64 - 1: seen by both cores,
 * whatever their clocks; timer24() = low 32 bits of the time since it started (ETUDE §24) */
#define TIMER1_CUR0 0x400e0028u
void timer24_init(void);
static inline uint32_t timer24(void)
{
    return ~*(volatile uint32_t *)TIMER1_CUR0;
}
uint32_t millis(void);          /* milliseconds since the timer started (49 days) */
void udelay(uint32_t us);
void mdelay(uint32_t ms);
void wdt_kick(void);

unsigned adc_read(int ch);
int key_ladder(unsigned v);
int key_read(void);             /* current key of the ladder (Power excluded) */
int power_key(void);
int vbus(void);
int charging(void);
unsigned battery_mv(void);
unsigned battery_pct(unsigned mv);  /* a Li-ion curve: 0 to 100 */

void reboot(void);
void reboot_loader_usb(void);
void power_off(void);

/* start.c's vector table: the SysTick interrupt, and the fault screen of main.c */
void SysTick_Handler(void);
void fault_screen(const uint32_t *frame, uint32_t exc);

#endif
