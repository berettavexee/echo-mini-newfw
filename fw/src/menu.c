/* The main menu (MENU held) and the date and time screen */
#include "ui_priv.h"

/* ---- main menu ---- */
static const char *const m_names[M_COUNT] = {
    "Files", "Now playing", "Date and time", "ReplayGain: ", "USB disk, read only", "USB disk, read/write",
    "Install a firmware (.IMG)", "System screen", "USB loader mode (exit: RST)",
};

int main_menu(void)
{
    int sel = 0, top = 0, dirty = 1;
    for (;;) {
        if (dirty) {                            /* ROWS of the M_COUNT entries: scrolled */
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(4, 0, "Menu", TITLE_FG, BG);
            for (int i = top; i < M_COUNT && i < top + ROWS; i++) {
                uint16_t bg = i == sel ? SEL_BG : BG;
                int y = ROW_Y0 + 16 * (i - top);
                lcd_fill(0, y, LCD_W, 16, bg);
                char b[48];
                char *p = put_str(b, m_names[i]);
                if (i == M_RG)
                    p = put_str(p, rg_names[rg_mode]);
                *p = 0;
                lcd_text_clip(12, y, b, list_text_end(M_COUNT, ROWS), FG, bg);
            }
            list_scrollbar(ROW_Y0, ROWS, top, M_COUNT);
            dirty = 0;
        }
        sys_idle();
        ui_background();
        struct key_ev e;
        int ev = key_poll(&e), k = e.key;
        if (ev == EV_NONE) {
            cpu_wait();
            continue;
        }
        if (k == KEY_PLAY && ev == EV_PRESS && sel == M_RG) {
            rg_mode = (rg_mode + 1) % RG_MODES; /* album, track, off: changed in place */
            rg_apply();
            resume_save();
            dirty = 1;
        } else if (k == KEY_VOLUP && ev != EV_LONG) {
            list_move(&sel, &top, M_COUNT, ROWS, -1);
            dirty = 1;
        } else if (k == KEY_VOLDOWN && ev != EV_LONG) {
            list_move(&sel, &top, M_COUNT, ROWS, 1);
            dirty = 1;
        } else if (k == KEY_BACK || (k == KEY_MENU && ev == EV_PRESS))
            return -1;
        else if (k == KEY_PLAY && ev == EV_PRESS)
            return sel;
    }
}

/* ---- date and time: VOL+/VOL- change the field, PLAY the next one (the last one saves), BACK
 * the previous one (the first one cancels), MENU cancels. Playback goes on meanwhile. ---- */
void clock_screen(void)
{
    struct rtc_tm t;
    int ok = rtc_get(&t), f = 0, dirty = 1;
    if (ok < 0) {
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        lcd_text(8, 60, "no clock found (I2C1, 0xA2)", WARN, BG);
        mdelay(1500);
        return;
    }
    if (!ok)
        t.year = 2026, t.mon = 1, t.day = 1, t.hour = 12, t.min = 0;
    t.sec = 0;
    for (;;) {
        if (dirty) {
            static const uint8_t ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            int dmax = ml[t.mon - 1] + (t.mon == 2 && t.year % 4 == 0);
            if (t.day > dmax)
                t.day = (uint8_t)dmax;
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(4, 0, "Date and time", TITLE_FG, BG);
            unsigned v[5] = { t.year, t.mon, t.day, t.hour, t.min };
            static const uint8_t wd[5] = { 4, 2, 2, 2, 2 };
            static const char *const sep[5] = { "-", "-", "  ", ":", "" };
            int x = 8;
            for (int i = 0; i < 5; i++) {
                char b[6];
                for (int k = wd[i] - 1, n = (int)v[i]; k >= 0; k--, n /= 10)
                    b[k] = (char)('0' + n % 10);
                b[wd[i]] = 0;
                int w = lcd_text_width_x(b, 2);
                lcd_text_clip_x(x, 50, b, x + w, FG, i == f ? SEL_BG : BG, 2);
                x += w;
                if (*sep[i]) {
                    w = lcd_text_width_x(sep[i], 2);
                    lcd_text_clip_x(x, 50, sep[i], x + w, DIM, BG, 2);
                    x += w;
                }
            }
            lcd_text(4, 118, "VOL+/VOL-: change   PLAY: next, save", DIM, BG);
            lcd_text(4, 136, "BACK: previous, cancel   MENU: cancel", DIM, BG);
            if (!ok)
                lcd_text(4, 154, "the clock had stopped: not set", WARN, BG);
            dirty = 0;
        }
        sys_idle();
        ui_background();
        struct key_ev e;
        int ev = key_poll(&e), k = e.key;
        if (ev == EV_NONE) {
            cpu_wait();
            continue;
        }
        if (k == KEY_VOLUP || k == KEY_VOLDOWN) {
            int d = k == KEY_VOLUP ? 1 : -1;
            if (f == 0)
                t.year = (uint16_t)(t.year + d < 2024 ? 2099 : t.year + d > 2099 ? 2024 : t.year + d);
            else if (f == 1)
                t.mon = (uint8_t)((t.mon - 1 + 12 + d) % 12 + 1);
            else if (f == 2)
                t.day = (uint8_t)((t.day - 1 + 31 + d) % 31 + 1);
            else if (f == 3)
                t.hour = (uint8_t)((t.hour + 24 + d) % 24);
            else
                t.min = (uint8_t)((t.min + 60 + d) % 60);
            dirty = 1;
        } else if (k == KEY_PLAY && ev == EV_PRESS) {
            if (++f == 5) {
                int r = rtc_set(&t);
                lcd_fill(0, 0, LCD_W, LCD_H, BG);
                lcd_text(8, 60, r ? "the clock did not answer" : "clock set", r ? WARN : FG, BG);
                mdelay(800);
                clock_min = -1;
                return;
            }
            dirty = 1;
        } else if (k == KEY_BACK && ev == EV_PRESS) {
            if (f-- == 0)
                return;
            dirty = 1;
        } else if (k == KEY_MENU && ev == EV_PRESS)
            return;
    }
}
