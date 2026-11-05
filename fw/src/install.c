/* Firmware installer: the *.IMG files of the internal root, checked then written by update.c */
#include <stdint.h>
#include "board.h"
#include "fat.h"
#include "keys.h"
#include "lcd.h"
#include "log.h"
#include "sys.h"
#include "sys_priv.h"
#include "ui.h"
#include "update.h"

#define MAX_IMG 32
#define IMG_ROWS 8                              /* rows of 16 px from y = 18, above the help line */

struct img_list {
    struct fat_file f[MAX_IMG];
    int n;
};

static int is_img(const char *name)
{
    int n = 0;
    while (name[n])
        n++;
    return n > 4 && name[n - 4] == '.' && name[n - 3] == 'I' && name[n - 2] == 'M' && name[n - 1] == 'G';
}

static int collect(const struct fat_file *f, void *ctx)
{
    struct img_list *l = ctx;
    if (is_img(f->name) && f->size >= 0x200)
        l->f[l->n++] = *f;
    return l->n == MAX_IMG;
}

static void progress(int stage, uint32_t done, uint32_t total)
{
    const char *const what[] = {
        ui_tr("checking CRC", "正在检查 CRC", "CRCを確認中"),
        ui_tr("writing FW2", "正在写入 FW2", "FW2を書き込み中"),
        ui_tr("writing FW1", "正在写入 FW1", "FW1を書き込み中"),
    };
    char b[48], *p = put_str(b, what[stage]);
    p = put_str(p, " ");
    p = put_dec(p, (uint32_t)((uint64_t)done * 100 / total));
    p = put_str(p, " %");
    *p = 0;
    sys_line(112, b, FG);
    int w = (int)((uint64_t)done * (LCD_W - 8) / total);
    lcd_fill(4, 130, w, 6, stage ? WARN : TITLE);
}

static char *put_date(char *p, const struct fw_info *in)
{
    p = put_hex(p, in->year, 4);
    *p++ = '-';
    p = put_hex(p, in->month, 2);
    *p++ = '-';
    return put_hex(p, in->day, 2);
}

/* Returns when the user goes back; reboots after a successful install */
static void firmware_menu(void)
{
    static struct img_list l;
    struct fw_info info;
    char b[64], *p;
    int sel = 0, top = 0;

    l.n = 0;
    if (!storage_err && !volume_err)
        fat_list_root(collect, &l);
    for (;;) {
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        sys_line(0, ui_tr("Install firmware (*.IMG, internal root)", "安装固件（*.IMG，内部存储根目录）", "ファームウェアをインストール（*.IMG、内蔵ストレージのルート）"), TITLE);
        if (storage_err || volume_err)
            sys_line(32, ui_tr("internal volume not available", "内部存储不可用", "内蔵ストレージを利用できません"), WARN);
        else if (!l.n)
            sys_line(32, ui_tr("no .IMG file in the root folder", "根目录中没有 .IMG 文件", "ルートに.IMGファイルがありません"), WARN);
        if (l.n) {                              /* "selected/count" right-aligned */
            p = put_dec(b, (uint32_t)sel + 1);
            *p++ = '/';
            p = put_dec(p, (uint32_t)l.n);
            *p = 0;
            lcd_text(LCD_W - 4 - lcd_text_width(b), 0, b, DIM, BG);
        }
        for (int i = top; i < l.n && i < top + IMG_ROWS; i++) {
            uint16_t bg = i == sel ? SEL_BG : BG;
            int y = 18 + 16 * (i - top);
            p = put_str(b, l.f[i].name);
            p = put_str(p, "  ");
            p = put_dec(p, l.f[i].size >> 10);
            p = put_str(p, " KiB");
            *p = 0;
            lcd_fill(0, y, 4, 16, bg);
            lcd_text_clip(4, y, b, list_text_end(l.n, IMG_ROWS), FG, bg);
        }
        list_scrollbar(18, IMG_ROWS, top, l.n);
        sys_line(154, ui_tr("VOL+/VOL-: choose  PLAY: check  BACK", "音量键选择  播放键检查  返回键退出", "VOL+/-選択  PLAY確認  BACK戻る"), DIM);

        int k;
        while (!(k = key_edge()))
            wdt_kick();
        if (k == KEY_BACK)
            return;
        if (k == KEY_VOLUP)                     /* the list wraps around */
            list_move(&sel, &top, l.n, IMG_ROWS, -1);
        if (k == KEY_VOLDOWN)
            list_move(&sel, &top, l.n, IMG_ROWS, 1);
        if (k != KEY_PLAY || !l.n)
            continue;

        lcd_fill(0, 18, LCD_W, 152, BG);
        p = put_str(b, l.f[sel].name);
        *p = 0;
        sys_line(18, b, FG);
        int r = fw_check(&l.f[sel], &info, progress);
        log_printf("install: %s, %u bytes: check %d (%s)", l.f[sel].name, l.f[sel].size, r,
                   r ? upd_error(r) : "valid");
        if (r) {
            sys_line(112, upd_error(r), WARN);
            sys_line(154, ui_tr("BACK: return", "返回键：返回", "BACK：戻る"), DIM);
            while (key_edge() != KEY_BACK)
                wdt_kick();
            continue;
        }
        p = put_str(b, "valid image, ");
        p = put_date(p, &info);
        p = put_str(p, ", ");
        p = put_dec(p, info.fw_size >> 20);
        p = put_str(p, " MiB");
        *p = 0;
        sys_line(36, b, FG);
        sys_line(54, info.two_copies ? ui_tr("writes FW2, then FW1", "依次写入 FW2 和 FW1", "FW2、続いてFW1に書き込み") :
                 ui_tr("too big for FW2: writes FW1 only", "镜像过大：只写入 FW1", "FW2には大きすぎるためFW1のみ書き込み"), FG);
        unsigned mv = battery_mv();
        if (!vbus() && mv < 3600) {
            sys_line(80, ui_tr("battery too low: plug the USB cable", "电量过低：请连接 USB 线", "電池残量不足：USBケーブルを接続してください"), WARN);
            sys_line(154, ui_tr("BACK: return", "返回键：返回", "BACK：戻る"), DIM);
            while (key_edge() != KEY_BACK)
                wdt_kick();
            continue;
        }
        sys_line(80, ui_tr("do not power off during the install", "安装期间请勿关机", "インストール中は電源を切らないでください"), WARN);
        sys_line(154, ui_tr("PLAY: install   BACK: cancel", "播放键安装  返回键取消", "PLAYでインストール  BACKで取消"), DIM);
        while ((k = key_edge()) != KEY_PLAY && k != KEY_BACK)
            wdt_kick();
        if (k == KEY_BACK)
            continue;
        sys_line(154, "", DIM);
        log_printf("install: writing %s: fw_end %x, %u bytes, %s", l.f[sel].name, info.fw_end,
                   info.fw_size, info.two_copies ? "FW2 then FW1" : "FW1 only");
        log_flush();
        uint32_t t0 = millis();
        r = fw_install(&l.f[sel], &info, progress);
        log_printf("install: result %d (%s) in %u ms", r, r ? upd_error(r) : "ok", millis() - t0);
        log_flush();
        if (r) {
            sys_line(112, upd_error(r), WARN);
            sys_line(130, ui_tr("restart, then install again", "请重启后重新安装", "再起動してから再インストールしてください"), WARN);
            sys_line(154, ui_tr("BACK: return", "返回键：返回", "BACK：戻る"), DIM);
            while (key_edge() != KEY_BACK)
                wdt_kick();
            continue;
        }
        sys_line(112, ui_tr("installed: restarting", "安装完成：正在重启", "インストール完了：再起動中"), TITLE);
        mdelay(1500);
        clock_fast_ok();                        /* this loop skips sys_idle(): a restart on */
        reboot();                               /* purpose is not a failed 200 MHz attempt */
    }
}

void sys_install(void)
{
    firmware_menu();
}
