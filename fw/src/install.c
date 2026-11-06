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
#include "ui_priv.h"                            /* MQ_*: the marquee timings */

#define MAX_IMG 32
#define IMG_ROWS 8                              /* rows of 16 px from y = 18, above the help line */

struct img_list {
    struct fat_file f[MAX_IMG];
    char name[MAX_IMG][FAT_NAME_MAX];           /* the long names (0.99r37: they were 8.3) */
    int n;
};

static int is_img(const char *name)             /* ".IMG", any case */
{
    int n = 0;
    while (name[n])
        n++;
    return n > 4 && name[n - 4] == '.' && (name[n - 3] | 0x20) == 'i' &&
           (name[n - 2] | 0x20) == 'm' && (name[n - 1] | 0x20) == 'g';
}

static int collect(const struct fat_dirent *e, void *ctx)
{
    struct img_list *l = ctx;
    if (!(e->attr & FAT_ATTR_DIR) && is_img(e->name) && e->size >= 0x200) {
        fat_file_of(e, &l->f[l->n]);
        for (int i = 0; i < FAT_NAME_MAX; i++)
            l->name[l->n][i] = e->name[i];
        l->n++;
    }
    return l->n == MAX_IMG;
}

/* The selected row scrolls when too long (0.99r37), with the timings of the browser */
static struct {
    int on, off, width, y, xmax;
    uint32_t t;
    char text[FAT_NAME_MAX + 24];
} mq;

static void mq_tick(void)
{
    if (!mq.on)
        return;
    uint32_t now = ticks();
    if (now - mq.t < (mq.off ? MQ_STEP : MQ_PAUSE))
        return;
    mq.off += mq.off ? (int)((now - mq.t) / MQ_STEP) : 1;
    mq.t = now;
    if (mq.off >= mq.width + MQ_GAP)
        mq.off = 0;
    lcd_text_scroll(4, mq.y, mq.text, mq.xmax, mq.off, MQ_GAP, 1, FG, SEL_BG);
}

static void progress(int stage, uint32_t done, uint32_t total)
{
    static const char *const what[] = { "checking CRC", "writing FW2", "writing FW1" };
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
    static struct img_list l __attribute__((section(".screenram")));    /* 5 KiB */
    struct fw_info info;
    char b[FAT_NAME_MAX + 24], *p;
    int sel = 0, top = 0;

    l.n = 0;
    if (!storage_err && !volume_err)
        fat_list_dir(0, collect, &l);
    for (;;) {
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        sys_line(0, "Install firmware (*.IMG, internal root)", TITLE);
        if (storage_err || volume_err)
            sys_line(32, "internal volume not available", WARN);
        else if (!l.n)
            sys_line(32, "no .IMG file in the root folder", WARN);
        if (l.n) {                              /* "selected/count" right-aligned */
            p = put_dec(b, (uint32_t)sel + 1);
            *p++ = '/';
            p = put_dec(p, (uint32_t)l.n);
            *p = 0;
            lcd_text(LCD_W - 4 - lcd_text_width(b), 0, b, DIM, BG);
        }
        mq.on = 0;
        for (int i = top; i < l.n && i < top + IMG_ROWS; i++) {
            uint16_t bg = i == sel ? SEL_BG : BG;
            int y = 18 + 16 * (i - top), xmax = list_text_end(l.n, IMG_ROWS);
            p = put_str(b, l.name[i]);
            p = put_str(p, "  ");
            p = put_dec(p, l.f[i].size >> 10);
            p = put_str(p, " KiB");
            *p = 0;
            lcd_fill(0, y, 4, 16, bg);
            lcd_text_clip(4, y, b, xmax, FG, bg);
            if (i == sel && lcd_text_width(b) > xmax - 4) {
                for (int k = 0; k < (int)sizeof mq.text; k++)
                    mq.text[k] = b[k];
                mq.on = 1;
                mq.off = 0;
                mq.width = lcd_text_width(b);
                mq.y = y;
                mq.xmax = xmax;
                mq.t = ticks();
            }
        }
        list_scrollbar(18, IMG_ROWS, top, l.n);
        sys_line(154, "VOL+/VOL-: choose  PLAY: check  BACK", DIM);

        int k;
        while (!(k = key_edge())) {
            wdt_kick();
            mq_tick();
        }
        mq.on = 0;
        if (k == KEY_BACK)
            return;
        if (k == KEY_VOLUP)                     /* the list wraps around */
            list_move(&sel, &top, l.n, IMG_ROWS, -1);
        if (k == KEY_VOLDOWN)
            list_move(&sel, &top, l.n, IMG_ROWS, 1);
        if (k != KEY_PLAY || !l.n)
            continue;

        lcd_fill(0, 18, LCD_W, 152, BG);
        p = put_str(b, l.name[sel]);
        *p = 0;
        sys_line(18, b, FG);
        int r = fw_check(&l.f[sel], &info, progress);
        log_printf("install: %s, %u bytes: check %d (%s)", l.name[sel], l.f[sel].size, r,
                   r ? upd_error(r) : "valid");
        if (r) {
            sys_line(112, upd_title(r), WARN);
            sys_line(130, upd_hint(r), FG);
            sys_line(154, "BACK: return", DIM);
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
        sys_line(54, info.two_copies ? "writes FW2, then FW1" : "too big for FW2: writes FW1 only", FG);
        unsigned mv = battery_mv();
        if (!vbus() && mv < 3600) {
            p = put_str(b, "Battery too low (");
            p = put_dec(p, battery_pct(mv));
            p = put_str(p, " %)");
            *p = 0;
            sys_line(80, b, WARN);
            sys_line(98, "plug the USB cable", FG);
            log_printf("install: battery %u mV: refused", mv);
            sys_line(154, "BACK: return", DIM);
            while (key_edge() != KEY_BACK)
                wdt_kick();
            continue;
        }
        sys_line(80, "do not power off during the install", WARN);
        sys_line(154, "PLAY: install   BACK: cancel", DIM);
        while ((k = key_edge()) != KEY_PLAY && k != KEY_BACK)
            wdt_kick();
        if (k == KEY_BACK)
            continue;
        sys_line(154, "", DIM);
        log_printf("install: writing %s: fw_end %x, %u bytes, %s", l.name[sel], info.fw_end,
                   info.fw_size, info.two_copies ? "FW2 then FW1" : "FW1 only");
        log_flush();
        uint32_t t0 = millis();
        r = fw_install(&l.f[sel], &info, progress);
        log_printf("install: result %d (%s) in %u ms", r, r ? upd_error(r) : "ok", millis() - t0);
        log_flush();
        if (r) {
            sys_line(112, upd_title(r), WARN);
            sys_line(130, upd_hint(r), WARN);
            sys_line(154, "BACK: return", DIM);
            while (key_edge() != KEY_BACK)
                wdt_kick();
            continue;
        }
        sys_line(112, "installed: restarting", TITLE);
        mdelay(1500);
        clock_fast_ok();                        /* this loop skips sys_idle(): a restart on */
        reboot();                               /* purpose is not a failed 200 MHz attempt */
    }
}

void sys_install(void)
{
    firmware_menu();
}
