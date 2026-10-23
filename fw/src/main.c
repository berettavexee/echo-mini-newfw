/* Step 3, first image: boot, power, clocks, panel, keys and ADC on a test screen, and a
 * firmware installer reading *.IMG files from the root of the internal volume.
 * No sound, no USB. The only flash writes are the installer's, inside the firmware area. */
#include <stdint.h>
#include "audio.h"
#include "board.h"
#include "emmc.h"
#include "fat.h"
#include "font.h"
#include "i2c.h"
#include "lcd.h"
#include "log.h"
#include "msc.h"
#include "player.h"
#include "hifi.h"
#include "regs.h"
#include "rtc.h"
#include "sd.h"
#include "update.h"
#include "usb.h"
#include "version.h"
#include "sys.h"
#include "ui.h"

extern uint32_t boot_regs[4], boot_tag;

char *put_str(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    return p;
}

char *put_dec(char *p, uint32_t v)
{
    char t[10];
    int n = 0;
    do
        t[n++] = '0' + v % 10;
    while (v /= 10);
    while (n)
        *p++ = t[--n];
    return p;
}

char *put_hex(char *p, uint32_t v, int digits)
{
    while (digits--)
        *p++ = "0123456789abcdef"[(v >> (4 * digits)) & 15];
    return p;
}

/* One text line, padded with the background up to the right edge */
static void line(int y, const char *s, uint16_t fg)
{
    int x = lcd_text(4, y, s, fg, BG);
    lcd_fill(x, y, LCD_W - x, 16, BG);
}

/* Status lines are redrawn only when their text or colour changed: every pixel goes through
 * the VOP register, so a full line costs several milliseconds */
#define STATUS_LINES 5
static char status_text[STATUS_LINES][64];
static uint16_t status_fg[STATUS_LINES];

static void status_line(int i, int y, const char *s, uint16_t fg)
{
    int n = 0;
    while (s[n] && s[n] == status_text[i][n])
        n++;
    if (!s[n] && !status_text[i][n] && fg == status_fg[i])
        return;
    for (n = 0; (status_text[i][n] = s[n]); n++)
        ;
    status_fg[i] = fg;
    line(y, s, fg);
}

static void status_reset(void)
{
    for (int i = 0; i < STATUS_LINES; i++)
        status_text[i][0] = (char)0xff;
}

static const char *const key_names[] = { "-", "VOL-", "VOL+", "PLAY", "MENU", "BACK" };

static int fast, storage_err = 1, volume_err = 1, volume_dirty;

static void volume_init(void)
{
    volume_err = fat_mount(EMMC_FW1 + emmc.p);
    volume_dirty = volume_err ? 0 : fat_dirty();
    if (volume_err) {
        log_printf("volume: no FAT32 volume found (%d)", volume_err);
        return;
    }
    log_printf("volume: MBR/boot at %x, boot sector %x, FAT %x, data %x, %u sectors/cluster, "
               "%u clusters, state %s (%d)", vol.base, vol.part, vol.fat, vol.data, vol.spc,
               vol.clusters, volume_dirty ? "dirty" : "clean", volume_dirty);
    int r = log_attach();
    static const char *const why[] = { "", "no " LOG_FILE " in the root", "file fragmented",
                                       "file size outside 8 KiB..16 MiB", "outside the volume",
                                       "read error" };
    if (r)
        log_printf("log: not written to the volume: %s", why[-r < 6 ? -r : 0]);
    else
        log_printf("log: %s, %u sectors, next %u, wraps %u", LOG_FILE, logs.file_sectors,
                   logs.next, logs.wraps);
}

#ifdef SD_WALK
/* Emulator test of the card (ETUDE §36-37, make EXTRA=-DSD_WALK): set up, mounted, its root
 * listed in the log, every folder walked and every file hashed (sd_walk) */
static int sd_root_entry(const struct fat_dirent *e, void *ctx)
{
    int *n = ctx;
    if (*n < 12)
        log_printf("sd:   %s%s, %u bytes, cluster %u%s", e->name, e->attr & FAT_ATTR_DIR ? "/" : "",
                   e->size, e->cluster, e->contig ? " (contiguous)" : "");
    (*n)++;
    return 0;
}

/* every directory down to depth 3 listed, every file read through fat_pread and hashed (FNV-1a
 * over 32-bit words) */
static struct fat_dirent walk_e[8];
static int walk_n[4];

static int walk_cb(const struct fat_dirent *e, void *ctx)
{
    int depth = *(int *)ctx;
    if (walk_n[depth] < 8)
        walk_e[walk_n[depth]] = *e;
    walk_n[depth]++;
    return 0;
}

static void sd_walk(const struct fat_dir *d, int depth)
{
    static uint32_t buf[512];
    static struct fat_dirent list[4][8];
    walk_n[depth] = 0;
    if (fat_list(d, walk_cb, &depth))
        log_printf("sd: walk: read error");
    int n = walk_n[depth] < 8 ? walk_n[depth] : 8;
    for (int i = 0; i < n; i++)
        list[depth][i] = walk_e[i];
    log_printf("sd: walk depth %d: %d entries", depth, walk_n[depth]);
    for (int i = 0; i < n; i++) {
        const struct fat_dirent *e = &list[depth][i];
        if (e->attr & FAT_ATTR_DIR) {
            log_printf("sd: walk dir %s", e->name);
            struct fat_dir sub;
            fat_dir_of(e, &sub);
            if (depth < 3)
                sd_walk(&sub, depth + 1);
            continue;
        }
        struct fat_file f;
        struct fat_cursor c = { 0, 0 };
        fat_file_of(e, &f);
        uint32_t h = 2166136261u, pos = 0;
        while (pos < f.size) {
            uint32_t k = f.size - pos > sizeof buf ? sizeof buf : f.size - pos;
            for (uint32_t j = 0; j < sizeof buf / 4; j++)
                buf[j] = 0;
            if (fat_pread(&f, &c, pos, buf, k) != (int)k) {
                log_printf("sd: walk %s: read error at %u", e->name, pos);
                break;
            }
            for (uint32_t j = 0; j < (k + 3) / 4; j++)
                h = (h ^ buf[j]) * 16777619u;
            pos += k;
        }
        log_printf("sd: walk file %s: %u bytes, hash %x", e->name, f.size, h);
    }
}

static void sd_probe(void)
{
    if (!sd_present()) {
        log_printf("sd: no card");
        return;
    }
    uint32_t t0 = ticks();
    if (sd_init())
        return;
    uint32_t t1 = ticks();
    if (fat_mount_sd()) {
        log_printf("sd: no FAT32 or exFAT volume found");
        return;
    }
    log_printf("sd: ready in %u ms; %s volume at %u, %u clusters of %u sectors, FAT %u, "
               "data %u, root cluster %u (mounted in %u ms)", (t1 - t0) / 1000,
               sdvol.exfat ? "exFAT" : "FAT32", sdvol.part, sdvol.clusters, sdvol.spc, sdvol.fat,
               sdvol.data, sdvol.root, (ticks() - t1) / 1000);
    struct fat_dir root;
    int n = 0;
    fat_root(FAT_VOL_SD, &root);
    t1 = ticks();
    int r = fat_list(&root, sd_root_entry, &n);
    log_printf("sd: root: %d entries (%d), listed in %u ms", n, r, (ticks() - t1) / 1000);
    sd_walk(&root, 0);
}
#endif

static void storage_init(void)
{
    uint32_t total = 0;

    storage_err = emmc_init();
    if (storage_err) {
        log_printf("emmc: IDB not found (%d)", storage_err);
        return;
    }
    int r = emmc_sectors(&total);
    log_printf("emmc: %u sectors (%d); IDB copy %d: P %x R %x", total, r, emmc.idb_copy, emmc.p,
               emmc.r);
    volume_init();
#ifdef SD_WALK
    sd_probe();
#endif
}

static void draw_static(void)
{
    char b[64], *p;

    status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    line(0, FW_NAME " " FW_VERSION " (" FW_DATE ")", TITLE);
    p = put_str(b, "CPU ");
    p = put_dec(p, cpu_hz / 1000000);
    p = put_str(p, fast ? " MHz" : " MHz (fallback)");
    if (fast) {
        p = put_str(p, " adc ");
        p = put_dec(p, adc_cycles_24m);
        p = put_str(p, "/");
        p = put_dec(p, adc_cycles);
        p = put_str(p, " pll ");
        p = put_hex(p, pll_con1 & 0xffff, 4);
    }
    *p = 0;
    line(16, b, fast && cpu_hz == 200000000 ? FG : WARN);
    if (storage_err) {
        p = put_str(b, "eMMC/IDB error ");
        p = put_dec(p, -storage_err);
    } else {
        p = put_str(b, "IDB P ");
        p = put_hex(p, emmc.p, 5);
        p = put_str(p, " R ");
        p = put_hex(p, emmc.r, 5);
        if (volume_err)
            p = put_str(p, "  no FAT32 volume");
        else {
            p = put_str(p, "  vol @");
            p = put_hex(p, vol.part, 6);
            p = put_str(p, " ");
            p = put_dec(p, vol.spc / 2);
            p = put_str(p, "K");
            if (volume_dirty < 0)
                p = put_str(p, " ?");
            else if (volume_dirty) {
                p = put_str(p, " dirty ");     /* 1: boot sector (Linux), 2: FAT[1], 4: error */
                p = put_dec(p, (uint32_t)volume_dirty);
            } else
                p = put_str(p, " clean");
        }
    }
    *p = 0;
    line(32, b, storage_err || volume_err || volume_dirty ? WARN : FG);
    line(122, "PLAY: USB disk   VOL+: USB disk, writable", DIM);
    line(138, "MENU: install firmware   Power 2 s: off", DIM);
    line(154, "VOL- 3 s: USB loader   BACK: audio tests", DIM);
}

static uint32_t draw_us;                        /* last status refresh that drew something */

static void draw_status(void)
{
    char b[64], *p;
    unsigned k = adc_read(1), mv = battery_mv();

    p = put_str(b, "key ");
    p = put_str(p, key_names[key_ladder(k)]);
    p = put_str(p, " (adc ");
    p = put_dec(p, k);
    p = put_str(p, ")  power ");
    p = put_str(p, power_key() ? "ON" : "-");
    *p = 0;
    status_line(0, 48, b, FG);
    p = put_str(b, "battery ");
    p = put_dec(p, mv);
    p = put_str(p, " mV  usb ");
    p = put_str(p, vbus() ? "yes" : "no");
    p = put_str(p, "  chg ");
    p = put_dec(p, gpio_get(PIN_CHARGING));
    *p = 0;
    status_line(1, 64, b, mv && mv < 3400 ? WARN : FG);
    p = put_str(b, "jack 3.5 ");
    p = put_str(p, gpio_get(PIN_JACK35) ? "in" : "-");
    p = put_str(p, "  4.4 ");
    p = put_str(p, gpio_get(PIN_JACK44) ? "-" : "in");
    p = put_str(p, "  adc2 ");
    p = put_dec(p, adc_read(2));
    p = put_str(p, " adc3 ");
    p = put_dec(p, adc_read(3));
    *p = 0;
    status_line(2, 80, b, FG);
    uint32_t s = ticks() / 1000000;
    p = put_str(b, "up ");
    p = put_dec(p, s / 60);
    *p++ = ':';
    *p++ = '0' + s % 60 / 10;
    *p++ = '0' + s % 10;
    p = put_str(p, "   draw ");
    p = put_dec(p, draw_us / 1000);
    p = put_str(p, " ms   log ");
    if (logs.attached)
        p = put_str(p, "on");
    else {
        p = put_str(p, "off ");
        p = put_dec(p, (uint32_t)-logs.error);
    }
    *p = 0;
    status_line(3, 96, b, logs.attached ? DIM : WARN);
}

static void message(int y, const char *s, uint16_t fg)
{
    line(y, s, fg);
}

/* ---- keys ---- */
/* The ladder is RC filtered: pressing VOL-, VOL+ or Play sweeps channel 1 down through the
 * Menu range for a few ms. A key counts once its class has been the same for 30 ms. A key held
 * (0.53, ETUDE §34): a release counts after 60 ms (a loose contact of Play reads none for a few
 * ms; VOL+/VOL-: 30 ms, quick clicks leave 50-60 ms between them, 0.54), VOL+ <-> VOL- (the chord) after 100 ms, and another key not at all until a release (a loose
 * contact of Play sweeps up through Menu and Back: these read as Back presses in 0.52). */
#define KEY_SETTLE_US   30000
#define KEY_RELEASE_US  60000
#define KEY_CHORD_US   100000
unsigned key_adc;                               /* channel 1 when the key held was taken */

static const char *const key_lnames[] = { "none", "vol-", "vol+", "play", "menu", "back" };

const char *key_name(int k)
{
    return key_lnames[k];
}

int key_scan(void)
{
    static int raw = KEY_NONE, stable = KEY_NONE;
    static uint32_t since;
    unsigned v = adc_read(1);
    int k = key_ladder(v);
    uint32_t now = ticks();

    if (k != raw) {
        uint32_t held = now - since;
        if (raw != stable && stable != KEY_NONE && held >= KEY_SETTLE_US / 2)
            log_printf("keys: %s held: %s for %u ms ignored", key_lnames[stable], key_lnames[raw],
                       held / 1000);
        raw = k;
        since = now;
        return stable;
    }
    if (k == stable)
        return stable;
    uint32_t need = stable == KEY_NONE ? KEY_SETTLE_US :
                    k == KEY_NONE ? (stable == KEY_VOLUP || stable == KEY_VOLDOWN ?
                                     KEY_SETTLE_US : KEY_RELEASE_US) :
                    (stable == KEY_VOLUP || stable == KEY_VOLDOWN) &&
                    (k == KEY_VOLUP || k == KEY_VOLDOWN) ? KEY_CHORD_US : 0xffffffffu;
    if (now - since >= need) {
        stable = k;
        if (k != KEY_NONE)
            key_adc = v;
    }
    return stable;
}

/* One event per press (Power excluded) */
static int key_last;

static int key_event(void)
{
    int k = key_scan();
    if (k == key_last)
        return KEY_NONE;
    key_last = k;
    return k;
}

/* ---- firmware menu ---- */
#define MAX_IMG 32

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
    static const char *const what[] = { "checking CRC", "writing FW2", "writing FW1" };
    char b[48], *p = put_str(b, what[stage]);
    p = put_str(p, " ");
    p = put_dec(p, (uint32_t)((uint64_t)done * 100 / total));
    p = put_str(p, " %");
    *p = 0;
    message(112, b, FG);
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
    int sel = 0;

    l.n = 0;
    if (!storage_err && !volume_err)
        fat_list_root(collect, &l);
    for (;;) {
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        line(0, "Install firmware (*.IMG, internal root)", TITLE);
        if (storage_err || volume_err)
            line(32, "internal volume not available", WARN);
        else if (!l.n)
            line(32, "no .IMG file in the root folder", WARN);
        for (int i = 0; i < l.n; i++) {
            p = put_str(b, i == sel ? "> " : "  ");
            p = put_str(p, l.f[i].name);
            p = put_str(p, "  ");
            p = put_dec(p, l.f[i].size >> 10);
            p = put_str(p, " KiB");
            *p = 0;
            line(18 + 14 * i, b, i == sel ? FG : DIM);
        }
        line(154, "VOL+/VOL-: choose  PLAY: check  BACK", DIM);

        int k;
        while (!(k = key_event()))
            wdt_kick();
        if (k == KEY_BACK)
            return;
        if (k == KEY_VOLUP && l.n)              /* the list wraps around */
            sel = sel > 0 ? sel - 1 : l.n - 1;
        if (k == KEY_VOLDOWN && l.n)
            sel = sel < l.n - 1 ? sel + 1 : 0;
        if (k != KEY_PLAY || !l.n)
            continue;

        lcd_fill(0, 18, LCD_W, 152, BG);
        p = put_str(b, l.f[sel].name);
        *p = 0;
        line(18, b, FG);
        int r = fw_check(&l.f[sel], &info, progress);
        log_printf("install: %s, %u bytes: check %d (%s)", l.f[sel].name, l.f[sel].size, r,
                   r ? upd_error(r) : "valid");
        if (r) {
            message(112, upd_error(r), WARN);
            line(154, "BACK: return", DIM);
            while (key_event() != KEY_BACK)
                wdt_kick();
            continue;
        }
        p = put_str(b, "valid image, ");
        p = put_date(p, &info);
        p = put_str(p, ", ");
        p = put_dec(p, info.fw_size >> 20);
        p = put_str(p, " MiB");
        *p = 0;
        line(36, b, FG);
        line(54, info.two_copies ? "writes FW2, then FW1" : "too big for FW2: writes FW1 only", FG);
        unsigned mv = battery_mv();
        if (!vbus() && mv < 3600) {
            message(80, "battery too low: plug the USB cable", WARN);
            line(154, "BACK: return", DIM);
            while (key_event() != KEY_BACK)
                wdt_kick();
            continue;
        }
        line(80, "do not power off during the install", WARN);
        line(154, "PLAY: install   BACK: cancel", DIM);
        while ((k = key_event()) != KEY_PLAY && k != KEY_BACK)
            wdt_kick();
        if (k == KEY_BACK)
            continue;
        line(154, "", DIM);
        log_printf("install: writing %s: fw_end %x, %u bytes, %s", l.f[sel].name, info.fw_end,
                   info.fw_size, info.two_copies ? "FW2 then FW1" : "FW1 only");
        log_flush();
        uint32_t t0 = millis();
        r = fw_install(&l.f[sel], &info, progress);
        log_printf("install: result %d (%s) in %u ms", r, r ? upd_error(r) : "ok", millis() - t0);
        log_flush();
        if (r) {
            message(112, upd_error(r), WARN);
            message(130, "restart, then install again", WARN);
            line(154, "BACK: return", DIM);
            while (key_event() != KEY_BACK)
                wdt_kick();
            continue;
        }
        message(112, "installed: restarting", TITLE);
        mdelay(1500);
        reboot();
    }
}

/* ---- USB disk: the user volume, read only ---- */
static void usb_screen(void)
{
    char b[64], *p;

    p = put_str(b, usb.configured ? "configured, " : "waiting for the host, ");
    p = put_str(p, usb.high_speed ? "high speed" : "full speed");
    *p = 0;
    status_line(0, 36, b, usb.configured ? FG : DIM);
    p = put_str(b, "cmds ");
    p = put_dec(p, msc.commands);
    p = put_str(p, "  read ");
    p = put_dec(p, msc.read_sectors / 2048);
    p = put_str(p, "  written ");
    p = put_dec(p, msc.written_sectors / 2048);
    p = put_str(p, " MiB");
    *p = 0;
    status_line(1, 52, b, FG);
    p = put_str(b, "op ");
    p = put_hex(p, msc.last_op, 2);
    p = put_str(p, "  errors ");
    p = put_dec(p, msc.errors);
    p = put_str(p, "  resets ");
    p = put_dec(p, usb.resets);
    p = put_str(p, "  setups ");
    p = put_dec(p, usb.setups);
    *p = 0;
    status_line(2, 68, b, msc.errors ? WARN : DIM);
    p = put_str(b, "disk @");
    p = put_hex(p, msc.base, 6);
    p = put_str(p, " ");
    p = put_dec(p, msc.count / 2048);
    p = put_str(p, " MiB");
    *p = 0;
    status_line(3, 84, b, DIM);
}

/* writable: the host may write the user volume (and only it: emmc_write_user's fence) */
static void usb_mode(int writable)
{
    uint32_t total = 0;

    status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    line(0, writable ? "USB disk (read/write)" : "USB disk (read only)", writable ? WARN : TITLE);
    if (storage_err || volume_err || emmc_sectors(&total) || total <= vol.base) {
        line(36, "internal volume not available", WARN);
        mdelay(2000);
        return;
    }
    if (!vbus()) {
        line(36, "plug the USB cable", WARN);
        mdelay(2000);
        return;
    }
    line(154, "BACK: exit (eject on the computer first)", DIM);
    if (writable)
        line(122, "eject on the computer before unplugging", WARN);
    if (msc_start(vol.base, total - vol.base, writable)) {
        line(36, "write fence refused: volume inside the system area", WARN);
        log_printf("usb: write fence refused for %x + %u", vol.base, total - vol.base);
        mdelay(3000);
        return;
    }
    log_printf("usb: %s disk, sectors %x + %u", writable ? "read/write" : "read only", vol.base,
               total - vol.base);
    log_detach();                               /* the host owns the volume from here */
    usb_start();
    uint32_t t_draw = 0, t_key = 0, t_eject = 0, t0 = millis(), configured_at = 0;
    const char *why = "?";
    for (;;) {
        wdt_kick();
        msc_poll();
        uint32_t now = ticks();
        if (usb.configured && !configured_at) {
            configured_at = millis();
            log_printf("usb: configured after %u ms, %s speed", configured_at - t0,
                       usb.high_speed ? "high" : "full");
        }
        if (now - t_key > 10000) {
            t_key = now;
            if (!vbus()) {
                why = "cable unplugged";
                break;
            }
            if (key_event() == KEY_BACK) {
                why = "BACK";
                break;
            }
        }
        if (now - t_draw > 250000) {
            usb_screen();
            t_draw = now;
        }
        /* The host stopped the bus (udisksctl power-off, port disabled): a mounted volume is
         * polled every few seconds, so 5 s without activity means it is gone */
        if (usb.suspended && now - usb.suspend_since > 5000000) {
            why = "bus suspended 5 s";
            break;
        }
        if (msc.ejected) {
            if (!t_eject)
                t_eject = now;
            else if (now - t_eject > 500000) {
                why = "ejected";
                break;
            }
        }
    }
    usb_stop();
    log_printf("usb: exit (%s) after %u s: %u commands, read %u written %u sectors, %u errors, "
               "%u resets, %u setups", why, (millis() - t0) / 1000, msc.commands, msc.read_sectors,
               msc.written_sectors, msc.errors, usb.resets, usb.setups);
    volume_init();                              /* the host may have changed everything */
}

/* ---- audio test (steps 5a, 5b): core A plays the part of core B and streams a 1 kHz tone
 * into the ring ahead of the DMA; MENU walks through rates and rings, Power stalls the
 * producer for 500 ms (an underrun when the ring holds less than that) ---- */
static const struct { uint32_t rate; int ring, bits16; } audio_cfg[] = {
    { 44100, RING_HIGHRAM1, 0 }, { 44100, RING_HIGHRAM1, 1 }, { 48000, RING_HIGHRAM1, 0 },
    { 48000, RING_HIGHRAM1, 1 }, { 88200, RING_HIGHRAM1, 0 }, { 96000, RING_HIGHRAM1, 0 },
    { 176400, RING_HIGHRAM1, 0 }, { 192000, RING_HIGHRAM1, 0 }, { 192000, RING_HIGHRAM1, 1 },
    { 44100, RING_PMU, 0 }, { 44100, RING_HIGHRAM0, 0 },
    { 44100, RING_SYSRAM, 1 }, { 44100, RING_SYSRAM, 0 }, { 192000, RING_SYSRAM, 0 },
};
static const char *const ring_names[RING_COUNT] = { "hram1 64K", "pmu 48K", "hram0 64K", "sram 128K" };

static uint32_t ms_of(int32_t bytes)
{
    return bytes <= 0 ? 0 : (uint32_t)((uint64_t)bytes * 1000 / (audio.rate * audio.frame_bytes));
}

static void audio_screen(int tone, uint8_t level)
{
    char b[64], *p;

    p = put_str(b, "dac0 ");
    for (int i = 0; i < 4; i++)
        p = put_hex(p, audio.id[0][i], 2);
    p = put_str(p, audio.id_err[0] ? " ERR" : "");
    p = put_str(p, "  dac1 ");
    for (int i = 0; i < 4; i++)
        p = put_hex(p, audio.id[1][i], 2);
    p = put_str(p, audio.id_err[1] ? " ERR" : "");
    p = put_str(p, "  i2c err ");
    p = put_dec(p, audio.i2c_errors);
    *p = 0;
    status_line(0, 20, b, audio.id_err[0] || audio.id_err[1] || audio.i2c_errors ? WARN : DIM);
    p = put_dec(b, audio.rate / 1000);
    if (audio.rate % 1000) {
        *p++ = '.';
        p = put_dec(p, audio.rate % 1000 / 100);
    }
    p = put_str(p, audio.bits16 ? " kHz 16b " : " kHz 32b ");
    p = put_str(p, ring_names[audio.ring_sel]);
    p = put_str(p, "  tone ");
    p = put_str(p, tone ? "on" : "off");
    p = put_str(p, "  -");
    p = put_dec(p, level / 2);
    p = put_str(p, " dB");
    *p = 0;
    status_line(1, 40, b, FG);
    p = put_str(b, "fill ");
    p = put_dec(p, ms_of(audio_fill()));
    p = put_str(p, " ms  min ");
    p = put_dec(p, ms_of(audio.min_fill));
    p = put_str(p, " ms  dma ");
    p = put_hex(p, audio_dma_pos(), 5);
    p = put_str(p, audio_dma_err() ? " ERR" : "");
    *p = 0;
    status_line(2, 56, b, FG);
    p = put_str(b, "underruns: ring ");
    p = put_dec(p, audio.ring_ur);
    p = put_str(p, "  i2s ");
    p = put_dec(p, audio.i2s_ur);
    p = put_str(p, "  fifo ");
    p = put_dec(p, audio_i2s_fifo());
    *p = 0;
    status_line(3, 72, b, audio.ring_ur || audio.i2s_ur ? WARN : FG);
}

static void audio_log(const char *what)
{
    log_printf("audio: %s: %u Hz %u-bit %s, fill %u ms (min %u), underruns ring %u (%u ms) i2s %u, dma %x err %x, "
               "fifo %u, i2c errors %u", what, audio.rate, audio.bits16 ? 16 : 32,
               ring_names[audio.ring_sel],
               ms_of(audio_fill()), ms_of(audio.min_fill), audio.ring_ur, ms_of((int32_t)audio.ring_lost),
               audio.i2s_ur,
               audio_dma_pos(), audio_dma_err(), audio_i2s_fifo(), i2c_errors);
}

static void audio_test(void)
{
    int tone = 0, cfg = 0;
    uint8_t level = 0x50;                       /* -40 dB when the tone is on */

    status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    line(0, "Audio test (steps 5a/5b)", TITLE);
    line(20, "starting...", DIM);
    audio_start(audio_cfg[cfg].rate, audio_cfg[cfg].ring, audio_cfg[cfg].bits16);
    line(122, "PLAY: tone   VOL+/-: volume   MENU: next", DIM);
    line(138, "Power: 500 ms stall test", DIM);
    line(154, "BACK: stop and return", DIM);
    log_flush();
    uint32_t t_draw = 0, t_log = 0;
    for (;;) {
        wdt_kick();
        audio_service(tone, 0);
        log_poll();
        uint32_t now = ticks();
        int k = key_event();
        if (k == KEY_BACK)
            break;
        if (k == KEY_PLAY) {
            tone = !tone;
            if (tone)
                audio_volume(level, 1);
            else
                audio_volume(0xff, 1);
            log_printf("audio: tone %s, volume %x", tone ? "on" : "off", audio.vol);
        }
        if (k == KEY_MENU) {                    /* next rate / ring */
            audio_log("before the change");
            audio_stop();
            cfg = (cfg + 1) % (int)(sizeof audio_cfg / sizeof *audio_cfg);
            audio_start(audio_cfg[cfg].rate, audio_cfg[cfg].ring, audio_cfg[cfg].bits16);
            if (tone)
                audio_volume(level, 1);
            t_log = now;
        }
        if (k == KEY_VOLUP || k == KEY_VOLDOWN) {        /* also while the tone is off */
            if (k == KEY_VOLUP && level >= 4)
                level -= 4;                     /* louder, up to 0 dB (the DAC maximum) */
            else if (k == KEY_VOLDOWN && level < 0xfe - 4)
                level += 4;
            if (tone)
                audio_volume(level, 0);
            log_printf("audio: volume %x (-%u dB)", level, level / 2);
        }
        if (power_key()) {                      /* stall: the producer stops for 500 ms */
            uint32_t ur = audio.ring_ur;
            uint32_t t0 = ticks();
            while (ticks() - t0 < 500000) {
                audio_service(tone, 1);
                wdt_kick();
            }
            audio_service(tone, 0);
            log_printf("audio: 500 ms stall at %u Hz, %u-bit (ring %u ms): ring underruns +%u, "
                       "%u ms lost in total", audio.rate, audio.bits16 ? 16 : 32,
                       ms_of((int32_t)audio.ring_size), audio.ring_ur - ur,
                       ms_of((int32_t)audio.ring_lost));
            while (power_key())
                audio_service(tone, 0), wdt_kick();
        }
        if (now - t_draw > 200000) {
            audio_screen(tone, level);
            t_draw = now;
        }
        if (now - t_log > 2000000) {
            audio_log("stats");
            t_log = now;
        }
    }
    audio_log("end");
    audio_stop();
    log_flush();
}

static int is_ext(const char *name, const char *ext)
{
    int n = 0;
    while (name[n])
        n++;
    return n > 4 && name[n - 4] == '.' && name[n - 3] == ext[0] && name[n - 2] == ext[1] &&
           name[n - 1] == ext[2];
}

/* Files we can play: with our own codec image when there is one (own), else with a stock
 * module (module, codec): WAV, FLAC, MP3, Opus and Vorbis (ours), M4A/AAC (module 75, codec 2) */
#include "hifi_proto.h"
static const struct { const char *ext; uint32_t own; int module, codec; } play_types[] = {
    { "WAV", HP_CODEC_WAV, 76, 3 }, { "FLA", HP_CODEC_FLAC, 0, 0 }, { "OPU", HP_CODEC_OPUS, 0, 0 },
    { "OGG", HP_CODEC_VORBIS, 0, 0 },
    { "MP3", HP_CODEC_MP3, 73, 0 },
    { "M4A", 0, 75, 2 },
};

static int play_type(const char *name)
{
    for (unsigned i = 0; i < sizeof play_types / sizeof *play_types; i++)
        if (is_ext(name, play_types[i].ext))
            return (int)i;
    return -1;
}

/* ---- steps 5c, 5d: a file through a stock codec module on core B ---- */
static uint32_t play_t_draw;

static void play_screen(void)
{
    char b[64], *p;

    p = put_dec(b, player.rate);
    p = put_str(p, " Hz  ");
    p = put_dec(p, player.channels);
    p = put_str(p, " ch  ");
    p = put_dec(p, player.bps);
    p = put_str(p, " bits  -");
    p = put_dec(p, play_level / 2);
    p = put_str(p, " dB");
    *p = 0;
    status_line(0, 36, b, FG);
    p = put_dec(b, player.time_ms / 60000);
    *p++ = ':';
    p = put_dec(p, player.time_ms / 10000 % 6);
    p = put_dec(p, player.time_ms / 1000 % 10);
    p = put_str(p, " / ");
    p = put_dec(p, player.length_ms / 60000);
    *p++ = ':';
    p = put_dec(p, player.length_ms / 10000 % 6);
    p = put_dec(p, player.length_ms / 1000 % 10);
    *p = 0;
    status_line(1, 52, b, FG);
    /* DAC ring: what is queued for the DACs, its share of the ring, its lowest, the underruns */
    int32_t fill = audio_fill();
    p = put_str(b, "DAC ring ");
    p = put_dec(p, ms_of(fill));
    p = put_str(p, " ms ");
    p = put_dec(p, audio.ring_size ? (uint32_t)((uint64_t)(fill > 0 ? fill : 0) * 100 / audio.ring_size) : 0);
    p = put_str(p, "% (min ");
    p = put_dec(p, ms_of(audio.min_fill));
    p = put_str(p, ")  ur ");
    p = put_dec(p, audio.ring_ur);
    p = put_str(p, "/");
    p = put_dec(p, audio.i2s_ur);
    *p = 0;
    status_line(2, 68, b, audio.ring_ur || audio.i2s_ur ? WARN : FG);
    /* core B: clock, load and MHz used in the last 250 ms, its output ring */
    p = put_str(b, "core B ");
    p = put_dec(p, hifi_hz / 1000000);
    p = put_str(p, " MHz");
    if (player.b_mhz10 || player.b_load) {
        p = put_str(p, "  load ");
        p = put_dec(p, (player.b_load + 5) / 10);
        p = put_str(p, "% (");
        p = put_dec(p, (player.b_mhz10 + 5) / 10);
        p = put_str(p, " MHz)");
    }
    *p = 0;
    status_line(3, 84, b, player.b_load > 850 ? WARN : DIM);
    p = put_str(b, "core B out ");
    p = put_dec(p, player.b_out >> 10);
    p = put_str(p, " KiB");
    *p = 0;
    status_line(4, 100, b, DIM);
}

static int play_ui(void)
{
    log_poll();                                 /* the start-up trace fills the ring fast */
    if (audio.on && audio.vol == 0xff && player.buffers >= 4)
        audio_volume((uint8_t)play_level, 1);   /* fade in once a little audio is queued */
    int k = key_event();
    if (k == KEY_BACK)
        return 1;
    if (k == KEY_VOLUP || k == KEY_VOLDOWN) {
        if (k == KEY_VOLUP && play_level >= 4)
            play_level -= 4;
        else if (k == KEY_VOLDOWN && play_level < 0xfe - 4)
            play_level += 4;
        if (audio.vol != 0xff)
            audio_volume((uint8_t)play_level, 0);
    }
    if (ticks() - play_t_draw > 250000) {
        play_t_draw = ticks();
        play_screen();
    }
    return 0;
}

static void play_file(const struct fat_file *f)
{
    char b[64], *p;

    if (cpu_hz < 100000000) {                   /* the PLL is off: core B would run at 6 MHz */
        message(122, "24 MHz fallback: restart the player first", WARN);
        log_printf("player: refused in the 24 MHz fallback");
        mdelay(2000);
        return;
    }

    status_reset();
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    p = put_str(b, "Play ");
    p = put_str(p, f->name);
    if (play_types[play_type(f->name)].own)
        p = put_str(p, " (our codec)");
    else {
        p = put_str(p, " (module ");
        p = put_dec(p, (uint32_t)play_types[play_type(f->name)].module);
        p = put_str(p, ")");
    }
    *p = 0;
    line(0, b, TITLE);
    line(138, "VOL+/VOL-: volume", DIM);
    line(154, "BACK: stop", DIM);
    log_flush();
    int t = play_type(f->name);
    int r = play_types[t].own ? player_play_own(f, play_types[t].own, play_ui, 0)
                              : player_play(f, play_types[t].module, play_types[t].codec, play_ui);
    play_screen();
    audio_stop();
    if (r < 0) {
        p = put_str(b, "error ");
        p = put_dec(p, (uint32_t)-r);
        p = put_str(p, ": see NEWFWLOG.TXT");
        *p = 0;
        line(120, b, WARN);
    } else
        line(120, r ? "stopped" : "end of file", FG);
    line(154, "BACK: return", DIM);
    log_flush();
    while (key_event() != KEY_BACK)
        wdt_kick();
}

static int collect_wav(const struct fat_file *f, void *ctx)
{
    struct img_list *l = ctx;
    if (play_type(f->name) >= 0 && f->size >= 44)
        l->f[l->n++] = *f;
    return l->n == MAX_IMG;
}

/* Chain tests: every file of the root whose name starts with 'prefix' and that one of our codecs
 * plays, in name order, one after the other with PLAY_CHAIN (core B restarted or reloaded, DACs
 * kept when nothing changes); 'loop': again and again, WAV files left out (the long cycle, TEST*:
 * they tell nothing more, ETUDE §24). The log gets the
 * time of each step of each switch and the core B clock report of each track. BACK stops. */
static void chain_files(const struct img_list *l, const char *prefix, int loop)
{
    static struct fat_file ch[MAX_IMG];
    char b[64], *p;
    int n = 0;

    for (int i = 0; i < l->n; i++)
    {
        int k = 0;
        while (prefix[k] && l->f[i].name[k] == prefix[k])
            k++;
        uint32_t own = play_types[play_type(l->f[i].name)].own;
        if (!prefix[k] && own && !(loop && own == HP_CODEC_WAV))   /* the long cycle: no WAV */
            ch[n++] = l->f[i];
    }
    for (int i = 1; i < n; i++)                 /* insertion sort by name */
        for (int j = i; j > 0; j--) {
            const char *a = ch[j - 1].name, *z = ch[j].name;
            int k = 0;
            while (a[k] && a[k] == z[k])
                k++;
            if ((uint8_t)a[k] <= (uint8_t)z[k])
                break;
            struct fat_file t = ch[j - 1];
            ch[j - 1] = ch[j];
            ch[j] = t;
        }
    if (cpu_hz < 100000000 || !n) {
        message(122, n ? "24 MHz fallback: restart the player first" : "no such file in the root", WARN);
        mdelay(2000);
        return;
    }
    log_printf("chain: %d %s* files%s", n, prefix, loop ? ", in a loop" : "");
    int r = 0;
    for (int i = 0, round = 1; i < n && !r; i++) {
        if (loop && i == 0)
            log_printf("chain: round %d", round);
        status_reset();
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        p = put_str(b, loop ? "Cycle " : "Chain ");
        p = put_dec(p, (uint32_t)i + 1);
        p = put_str(p, "/");
        p = put_dec(p, (uint32_t)n);
        p = put_str(p, ": ");
        p = put_str(p, ch[i].name);
        *p = 0;
        line(0, b, TITLE);
            line(138, "VOL+/VOL-: volume", DIM);
        line(154, "BACK: stop", DIM);
        r = player_play_own(&ch[i], play_types[play_type(ch[i].name)].own, play_ui, PLAY_CHAIN);
        if (loop && !r && i == n - 1) {
            i = -1;                             /* the long cycle starts again */
            round++;
        }
    }
    player_chain_end();
    audio_stop();
    log_printf("chain: %s", r ? "stopped" : "done");
    log_flush();
    message(122, r ? "chain stopped" : "chain done", FG);
    mdelay(1500);
}

/* BACK on the main screen: the tone test, or a WAV, FLAC, Opus, Ogg Vorbis, MP3 or M4A of the
 * root through core B */
static void tests_menu(void)
{
    static struct img_list l;
    char b[64], *p;
    int sel = 0;

    l.n = 0;
    if (!storage_err && !volume_err)
        fat_list_root(collect_wav, &l);
    for (;;) {
        lcd_fill(0, 0, LCD_W, LCD_H, BG);
        static const uint8_t clk_modes[] = { 0, 8, 4, 3, 2 };
        static const char *const clk_names[] = { "auto", "50", "100", "133", "200" };
        int cm = 0;
        for (unsigned i = 0; i < sizeof clk_modes; i++)
            if (clk_modes[i] == player_clock)
                cm = (int)i;
        p = put_str(b, "Audio tests   core B: ");
        p = put_str(p, clk_names[cm]);
        p = put_str(p, cm ? " MHz" : "");
        *p = 0;
        line(0, b, TITLE);
        static int top;
        if (sel < top)
            top = sel;
        if (sel > top + 8)
            top = sel - 8;
        for (int i = top; i <= l.n + 2 && i <= top + 8; i++) {
            p = put_str(b, i == sel ? "> " : "  ");
            if (!i)
                p = put_str(p, "tone, rates, rings (5a/5b)");
            else if (i == l.n + 1)
                p = put_str(p, "chain the CH* files");
            else if (i == l.n + 2)
                p = put_str(p, "cycle the TEST* files (long, BACK stops)");
            else {
                p = put_str(p, "play ");
                p = put_str(p, l.f[i - 1].name);
                p = put_str(p, "  ");
                p = put_dec(p, l.f[i - 1].size >> 10);
                p = put_str(p, " KiB");
            }
            *p = 0;
            line(18 + 14 * (i - top), b, i == sel ? FG : DIM);
        }
        line(154, "VOL+/-: choose PLAY: start MENU: clock BACK", DIM);
        int k;
        while (!(k = key_event()))
            wdt_kick();
        if (k == KEY_BACK)
            return;
        if (k == KEY_MENU) {
            player_clock = clk_modes[(cm + 1) % (int)sizeof clk_modes];
            log_printf("tests: core B clock %s", player_clock ? "fixed" : "automatic");
            continue;
        }
        if (k == KEY_VOLUP)
            sel = sel > 0 ? sel - 1 : l.n + 2;
        if (k == KEY_VOLDOWN)
            sel = sel < l.n + 2 ? sel + 1 : 0;
        if (k != KEY_PLAY)
            continue;
        if (!sel)
            audio_test();
        else if (sel == l.n + 1)
            chain_files(&l, "CH", 0);
        else if (sel == l.n + 2)
            chain_files(&l, "TEST", 1);
        else
            play_file(&l.f[sel - 1]);
    }
}

/* ---- for the user interface (sys.h) ---- */
static uint32_t sys_t_start, sys_t_bat, sys_t_power;
static int sys_fast_cleared, sys_power_released, sys_screen_off;
static uint32_t a_idle;                         /* TIMER1 ticks asleep in cpu_wait() */

void sys_idle(void)
{
    wdt_kick();
    uint32_t now = ticks();
    if (!sys_fast_cleared && now - sys_t_start > 5000000) {
        /* 5 s without trouble: at 200 MHz, the attempt is good; in the 24 MHz fallback, the
         * marker must go too, or every later boot would stay in the fallback (0.23) */
        clock_fast_ok();
        sys_fast_cleared = 1;
    }
    log_poll();
    rtc_service();
    if (millis() - sys_t_bat > 60000) {
        static uint32_t t_win, idle_win;
        struct rtc_tm t;
        int c = rtc_get(&t);
        uint32_t now24 = timer24(), el = now24 - t_win, sl = a_idle - idle_win;
        t_win = now24;
        idle_win = a_idle;
        sys_t_bat = millis();
        log_printf("battery %u mV, usb %d, charge pin %d, font reads %u, clock %02u:%02u:%02u%s, "
                   "core A busy %u.%u %%", battery_mv(), vbus(), gpio_get(PIN_CHARGING),
                   font_misses(), c < 0 ? 0 : t.hour, c < 0 ? 0 : t.min, c < 0 ? 0 : t.sec,
                   c == 1 ? "" : c ? " (not set)" : " (none)",
                   el > sl ? (uint32_t)((uint64_t)(el - sl) * 1000 / el) / 10 : 0,
                   el > sl ? (uint32_t)((uint64_t)(el - sl) * 1000 / el) % 10 : 0);
    }
    /* Power: a short press switches the screen off and on, 2 s switch the player off */
    if (!sys_power_released) {
        sys_power_released = !power_key();      /* ignore the press that switched us on */
    } else if (power_key()) {
        if (!sys_t_power)
            sys_t_power = now ? now : 1;
        else if (now - sys_t_power > 2000000) {
            sys_screen_set(1);
            message(122, "power off", WARN);
            sys_power_off("Power 2 s");
            message(122, "power off: unplug the USB cable", WARN);
            while (power_key())
                wdt_kick();
            mdelay(200);
            while (!power_key() && !key_read())
                wdt_kick();
            reboot();
        }
    } else if (sys_t_power) {
        if (now - sys_t_power > 30000)          /* a press, not a bounce */
            sys_screen_set(sys_screen_off);
        sys_t_power = 0;
    }
}

/* The state saved, then the power hold released; returns when the USB cable still powers us */
void sys_power_off(const char *why)
{
    ui_power_off();
    log_printf("power off (%s), battery %u mV", why, battery_mv());
    log_flush();
    mdelay(300);
    power_off();
    log_printf("power off: still powered (USB)");
}

/* WFI only while the 1 ms SysTick interrupt runs, so that the wait is bounded. A function of
 * its own: the emulator (tools/emu_fw.py), which has no SysTick, replaces it by a return. */
__attribute__((noinline)) void cpu_wait(void)
{
    if (REG(SYST_CSR) & 2) {
        uint32_t t0 = timer24();
        __asm volatile ("wfi");
        a_idle += timer24() - t0;
    }
}

int sys_screen_is_off(void)
{
    return sys_screen_off;
}

void sys_screen_set(int on)
{
    if (on == !sys_screen_off)
        return;
    sys_screen_off = !on;
    if (on) {
        if (clock_is_low)
            clock_low(0);                       /* the screen at full speed (ui.c power_service) */
        lcd_sleep(0);
        lcd_backlight(1);
    } else {
        lcd_backlight(0);
        lcd_sleep(1);
    }
    log_printf("screen %s", on ? "on" : "off");
}

static int newest_write(const struct fat_dirent *e, void *ctx)
{
    uint32_t *m = ctx;
    if (e->mtime > *m)
        *m = e->mtime;
    return 0;
}

/* After a read/write session the clock may be set from what the computer wrote (rtc_from_fat) */
void sys_usb(int writable)
{
    usb_mode(writable);
    uint32_t newest = 0;
    if (writable && !volume_err && !fat_list_dir(0, newest_write, &newest) && newest)
        rtc_from_fat(newest);
}

void sys_install(void)
{
    firmware_menu();
}

void sys_tests(void)
{
    tests_menu();
}

void sys_screen(void)
{
    draw_static();
    draw_status();
    key_last = key_scan();
    for (uint32_t t_draw = 0;;) {
        sys_idle();
        if (ticks() - t_draw > 50000) {
            draw_status();
            t_draw = ticks();
        }
        if (key_event())
            return;
    }
}

void sys_loader_usb(void)
{
    message(122, "USB loader mode (exit: RST)", WARN);
    log_printf("reboot to the loader USB mode (main menu)");
    log_flush();
    mdelay(1000);
    reboot_loader_usb();
}

int main(void)
{
    gpio_set(PIN_POWER_HOLD, 1);                /* the loader set it already; keep it */
    gpio_dir(PIN_POWER_HOLD, 1);
    iomux(PIN_POWER_KEY, 0);
    gpio_dir(PIN_POWER_KEY, 0);
    gpio_pull(PIN_JACK35, 0);                   /* externally driven: RECHO turns the pull off */
    gpio_dir(PIN_JACK35, 0);
    gpio_dir(2, 8, 0);
    gpio_dir(2, 3, 0);

    fast = clock_init();
    timer_init();
    timer24_init();
    log_init();
    log_printf("=== boot %u: " FW_NAME " " FW_VERSION " (" FW_DATE "), tag %x, r0 %x r2 %x",
               logs.boots, boot_tag & 0xffff, boot_regs[0], boot_regs[2]);
    if (logs.recovered)
        log_printf("log: %u bytes of the previous boot recovered from PMU-SRAM", logs.recovered);
    log_printf("clock: %u MHz%s, mode %x sel0 %x sel1 %x pll %x, tries %u, adc %u/%u",
               cpu_hz / 1000000, fast ? "" : " (fallback: 24 MHz)", cru_mode, cru_sel0, cru_sel1,
               pll_con1_after & 0xffff, mode_tries, adc_cycles_24m, adc_cycles);
    {
        uint32_t t0 = timer24(), u0 = ticks();
        udelay(1000);
        log_printf("timer: TIMER1 %u ticks in %u us (24 expected per us)", timer24() - t0, ticks() - u0);
    }
    lcd_init();
    lcd_text(4, 0, "starting...", DIM, BG);
    lcd_backlight(1);
    storage_init();
    font_init();
    rtc_init();
    log_printf("battery %u mV, usb %d, charge pin %d, jack 3.5 %d 4.4 %d", battery_mv(), vbus(),
               gpio_get(PIN_CHARGING), gpio_get(PIN_JACK35), !gpio_get(PIN_JACK44));
    log_flush();
    draw_static();
    draw_status();

    sys_t_start = ticks();
    sys_t_bat = millis();
    ui_main();
}

/* Called by the fault handlers: exception number and stacked frame (r0-r3, r12, lr, pc, psr) */
void fault_screen(const uint32_t *frame, uint32_t exc)
{
    char b[64], *p;

    __asm volatile ("cpsid i");
    lcd_wake_fault();
    /* into the PMU-SRAM ring only: the next boot writes it to the file */
    log_printf("FAULT exc %u pc %x lr %x psr %x cfsr %x hfsr %x bfar %x", exc,
               frame ? frame[6] : 0, frame ? frame[5] : 0, frame ? frame[7] : 0,
               REG(0xe000ed28), REG(0xe000ed2c), REG(0xe000ed38));
    if (!lcd_ready) {
        power_off();
        reboot();
    }
    lcd_fill(0, 0, LCD_W, LCD_H, RGB(128, 0, 0));
    p = put_str(b, "FAULT exc ");
    p = put_dec(p, exc);
    p = put_str(p, " cfsr ");
    p = put_hex(p, REG(0xe000ed28), 8);
    *p = 0;
    lcd_text(4, 4, b, FG, RGB(128, 0, 0));
    if (frame) {
        p = put_str(b, "pc ");
        p = put_hex(p, frame[6], 8);
        p = put_str(p, " lr ");
        p = put_hex(p, frame[5], 8);
        *p = 0;
        lcd_text(4, 24, b, FG, RGB(128, 0, 0));
    }
    p = put_str(b, "bfar ");
    p = put_hex(p, REG(0xe000ed38), 8);
    *p = 0;
    lcd_text(4, 44, b, FG, RGB(128, 0, 0));
    lcd_text(4, 84, "any key: restart   Power 2 s: off", FG, RGB(128, 0, 0));
    lcd_backlight(1);
    /* no SysTick here: busy-wait on the ADC (about 20 us per read) */
    for (unsigned n = 0;;) {
        wdt_kick();
        if (power_key()) {
            if (++n > 100000)
                power_off();
        } else if (n) {
            reboot();
        } else if (key_read())
            reboot();
    }
}
