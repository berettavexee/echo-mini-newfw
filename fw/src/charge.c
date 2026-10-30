/* Charging screen (ETUDE §48): the USB cable switched the player on, or Power was held with the
 * cable in. A battery bar from the voltage (the generic curve of board.c; a little high while
 * charging) and the charger's own state: "Charging" while it pulls its pin low, "Charged" after.
 * No resume, no codec, no USB disk. The screen goes off after SCREEN_TIMEOUT_US like the others
 * (then the clocks on the crystal); a short press of any key wakes it; PLAY held or Power held
 * 1 s starts the player as usual; the cable out powers it off. */
#include <stdint.h>
#include "audio.h"
#include "board.h"
#include "hifi.h"
#include "keys.h"
#include "lcd.h"
#include "log.h"
#include "sys.h"

#define CH_SEGS      10
#define CH_X         60                         /* the battery outline */
#define CH_Y         34
#define CH_W         192
#define CH_H         70
#define CH_FG        RGB(90, 200, 90)
#define POWER_START_US 1000000u
#define CABLE_OUT_US   500000u
#define BOOT_NOTE_START 0x43485247u             /* "GRHC": restart without this screen */

int sys_power_mine;

static void draw_frame(void)
{
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    lcd_fill(CH_X, CH_Y, CH_W, 3, FG);
    lcd_fill(CH_X, CH_Y + CH_H - 3, CH_W, 3, FG);
    lcd_fill(CH_X, CH_Y, 3, CH_H, FG);
    lcd_fill(CH_X + CH_W - 3, CH_Y, 3, CH_H, FG);
    lcd_fill(CH_X + CH_W, CH_Y + 20, 10, CH_H - 40, FG);       /* the terminal */
    const char *h = "PLAY or Power held: start";
    lcd_text((LCD_W - lcd_text_width(h)) / 2, 150, h, DIM, BG);
}

/* The bar, the last segment blinking while charging, and the text under it */
static void draw_level(unsigned pct, int full, int blink)
{
    int segs = full ? CH_SEGS : (int)(pct * CH_SEGS + 50) / 100, sw = (CH_W - 14) / CH_SEGS;
    int x0 = CH_X + (CH_W - CH_SEGS * sw + 3) / 2;
    if (!full && segs < 1)
        segs = 1;
    for (int i = 0; i < CH_SEGS; i++) {
        int on = i < segs && !(blink && !full && i == segs - 1);
        lcd_fill(x0 + i * sw, CH_Y + 7, sw - 3, CH_H - 14, on ? CH_FG : BG);
    }
    char b[24], *p = put_str(b, full ? "Charged " : "Charging ");
    p = put_dec(p, full ? 100 : pct > 99 ? 99 : pct);     /* 100: when the charger says so */
    p = put_str(p, "%");
    *p = 0;
    lcd_fill(0, 116, LCD_W, 26, BG);
    lcd_text_clip_x((LCD_W - lcd_text_width_x(b, 2)) / 2, 116, b, LCD_W, FG, BG, 2);
}

static void start(int at_boot, const char *how)
{
    log_printf("charge: start (%s held)", how);
    sys_power_mine = 0;
    sys_screen_set(1);
    if (at_boot)
        return;
    log_boot_note(BOOT_NOTE_START);             /* PLAY may be released by then: not this */
    log_flush();                                /* screen again at the next boot */
    clock_fast_ok();                            /* a restart on purpose: not a failed 200 MHz */
    reboot();                                   /* attempt (no 24 MHz fallback after it) */
}

void charge_screen(int at_boot)
{
    if (at_boot && (log_boot_note_take() == BOOT_NOTE_START || !vbus() || power_key()))
        return;                                 /* switched on by Power, or restarted from here */
    log_printf("charge: screen (%s, power key %d), battery %u mV, charge pin %d",
               at_boot ? "powered on by the USB cable" : "Power held with the cable in",
               power_key(), battery_mv(), gpio_get(PIN_CHARGING));
    if (audio.on)                               /* after Power held: the player was running */
        audio_stop();
    hifi_sleep();
    if (!at_boot) {                             /* power_off() let both go: on again, or */
        gpio_set(PIN_POWER_HOLD, 1);            /* Power would restart the whole player */
        lcd_backlight(1);                       /* (sys_screen_set sees the screen as on) */
    }
    sys_power_mine = 1;
    sys_screen_set(1);
    draw_frame();
    uint32_t mv = battery_mv(), t_key = ticks(), t_draw = 0, t_power = 0, t_out = 0;
    int full = !charging(), shown_full = -1, blink = 0, released = !power_key();
    struct key_ev e;
    while (key_poll(&e))                        /* what was pending before this screen */
        ;
    for (;;) {
        sys_idle();
        uint32_t now = ticks();
        int wake = 0;
        while (key_poll(&e)) {
            if (e.type == EV_LONG && e.key == KEY_PLAY) {
                start(at_boot, "PLAY");
                return;
            }
            if (e.type == EV_PRESS)
                wake = 1;
        }
        if (!released) {
            released = !power_key();            /* the press that brought us here */
        } else if (power_key()) {
            if (!t_power)
                t_power = now ? now : 1;
            else if (now - t_power > POWER_START_US) {
                start(at_boot, "Power");
                return;
            }
        } else if (t_power) {
            if (now - t_power > 30000)          /* a press, not a bounce */
                wake = 1;
            t_power = 0;
        }
        if (!vbus()) {
            if (!t_out)
                t_out = now ? now : 1;
            else if (now - t_out > CABLE_OUT_US) {
                log_printf("charge: power off (cable out), battery %u mV", battery_mv());
                log_flush();
                mdelay(300);
                power_off();
                log_printf("charge: still powered");
                t_out = 0;
            }
        } else
            t_out = 0;
        if (wake) {
            t_key = now;
            if (sys_screen_is_off()) {
                sys_screen_set(1);
                draw_frame();
                t_draw = 0;
                shown_full = -1;
            }
        }
        if (now - t_draw > 500000) {            /* twice a second: the blink, the level */
            t_draw = now;
            mv = (mv * 7 + battery_mv()) / 8;
            int f = !charging();
            if (f != full && vbus()) {
                full = f;
                log_printf("charge: %s, battery %u mV", full ? "charged" : "charging again", mv);
            }
            blink ^= 1;
            if (!sys_screen_is_off() && (!full || shown_full != full)) {
                draw_level(battery_pct(mv), full, blink);
                shown_full = full;
            }
        }
        if (!sys_screen_is_off() && now - t_key > SCREEN_TIMEOUT_US) {
            sys_screen_set(0);
            if (!clock_is_low && cpu_hz > 24000000)
                clock_low(1);                   /* sys_screen_set(1) puts the PLL back */
        }
        cpu_wait();
    }
}
