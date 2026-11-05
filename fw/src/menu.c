/* The main menu (MENU held) and the date and time screen */
#include "ui_priv.h"

/* ---- main menu ---- */
static const char *menu_name(int i)
{
    switch (i) {
    case M_FILES: return ui_tr("Files", "文件", "ファイル");
    case M_PLAYING: return ui_tr("Now playing", "正在播放", "再生中");
    case M_SETTINGS: return ui_tr("Settings", "设置", "設定");
    case M_USB_RO: return ui_tr("USB disk, read only", "USB 磁盘（只读）", "USBディスク（読み取り専用）");
    case M_USB_RW: return ui_tr("USB disk, read/write", "USB 磁盘（读写）", "USBディスク（読み書き）");
    case M_USB_DAC: return ui_tr("USB sound card", "USB 声卡", "USB DAC");
    case M_INSTALL: return ui_tr("Install a firmware (.IMG)", "安装固件（.IMG）", "ファームウェアをインストール（.IMG）");
    case M_SYSTEM: return ui_tr("System screen", "系统信息", "システム情報");
    default: return ui_tr("USB loader mode (exit: RST)", "USB 引导模式（RST 退出）", "USBローダーモード（RSTで終了）");
    }
}

int main_menu(void)
{
    int sel = 0, top = 0, dirty = 1;
    for (;;) {
        if (dirty) {                            /* ROWS of the M_COUNT entries: scrolled */
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(4, 0, ui_tr("Menu", "菜单", "メニュー"), TITLE_FG, BG);
            for (int i = top; i < M_COUNT && i < top + ROWS; i++) {
                uint16_t bg = i == sel ? SEL_BG : BG;
                int y = ROW_Y0 + 16 * (i - top);
                lcd_fill(0, y, LCD_W, 16, bg);
                lcd_text_clip(12, y, menu_name(i), list_text_end(M_COUNT, ROWS), FG, bg);
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
        if (k == KEY_VOLUP && ev != EV_LONG) {
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
    int ok = rtc_get(&t), f = 0, dirty = 1, show = cfg.show_clock;
    if (ok < 0) {
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        lcd_text_clip(8, 60, ui_tr("no clock found (I2C1, 0xA2)", "未检测到时钟（I2C1, 0xA2）", "時計が見つかりません（I2C1, 0xA2）"), LCD_W, WARN, BG);
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
            lcd_text(4, 0, ui_tr("Date and time", "日期和时间", "日時"), TITLE_FG, BG);
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
            char sw[40], *sp = sw;
            sp = put_str(sp, ui_tr("Show clock: ", "显示时钟：", "時計表示："));
            sp = put_str(sp, show ? ui_tr("on", "开", "オン") : ui_tr("off", "关", "オフ"));
            *sp = 0;
            lcd_text_clip(8, 90, sw, LCD_W, f == 5 ? FG : DIM, f == 5 ? SEL_BG : BG);
            lcd_text_clip(4, 118, ui_tr("VOL+/VOL-: change   PLAY: next, save", "音量键调整  播放键下一项/保存", "VOL+/-変更  PLAY次へ/保存"), LCD_W, DIM, BG);
            lcd_text_clip(4, 136, ui_tr("BACK: previous, cancel   MENU: cancel", "返回键上一项/取消  菜单键取消", "BACK前へ/取消  MENU取消"), LCD_W, DIM, BG);
            if (!ok)
                lcd_text_clip(4, 154, ui_tr("the clock had stopped: not set", "时钟已停止：尚未设置", "時計が停止していました：未設定"), LCD_W, WARN, BG);
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
            else if (f == 4)
                t.min = (uint8_t)((t.min + 60 + d) % 60);
            else
                show ^= 1;
            dirty = 1;
        } else if (k == KEY_PLAY && ev == EV_PRESS) {
            if (++f == 6) {
                cfg.show_clock = (uint8_t)show;
                resume_save();
                int r = rtc_set(&t);
                lcd_fill(0, 0, LCD_W, LCD_H, BG);
                lcd_text_clip(8, 60, r ? ui_tr("the clock did not answer", "时钟无响应", "時計が応答しません") : ui_tr("clock set", "时间已设置", "時刻を設定しました"), LCD_W, r ? WARN : FG, BG);
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
