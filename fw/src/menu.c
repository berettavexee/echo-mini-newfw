/* The main menu (MENU held) and the date and time screen */
#include "ui_priv.h"
#include "board.h"
#include "fat.h"
#include "version.h"

/* ---- main menu (ETUDE §151): seven entries with icons; USB and System open a submenu. A table
 * per menu; each line: an icon (main menu), its name, at the right in grey what it holds, ">"
 * for a submenu; greyed (and PLAY does nothing) when it cannot be used: nothing to play, no USB
 * cable, no firmware image. ---- */
static const uint8_t mi_files[] = { 0x12, 0x10, 0x0c, 0x00, 0xff, 0x78, 0x00, 0x84, 0x00, 0x83,
                                    0xfe, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80,
                                    0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x7f, 0xfe };
static const uint8_t mi_playing[] = { 0x12, 0x10, 0x0c, 0x00, 0xff, 0x01, 0xfc, 0x01, 0x04, 0x01,
                                      0xfc, 0x01, 0x04, 0x01, 0x04, 0x01, 0x04, 0x01, 0x04, 0x0f,
                                      0x3c, 0x1f, 0x7c, 0x1f, 0x7c, 0x0e, 0x38, 0x00, 0x00 };
static const uint8_t mi_queue[] = { 0x12, 0x10, 0x0c, 0x00, 0xff, 0x80, 0x00, 0xcf, 0xfc, 0xe0,
                                    0x00, 0xcf, 0xfc, 0x80, 0x00, 0x00, 0x00, 0x0f, 0xfc, 0x00,
                                    0x00, 0x0f, 0xfc, 0x00, 0x00, 0x0f, 0xfc, 0x00, 0x00 };
static const uint8_t mi_usb[] = { 0x12, 0x10, 0x07, 0x00, 0x02, 0x07, 0xfc, 0x0c, 0x44, 0x38,
                                  0x54, 0x38, 0x44, 0x38, 0x54, 0x0c, 0x44, 0x07, 0xfc };
                                                /* a plug (the user's drawing, §152) */
static const uint8_t mi_settings[] = { 0x12, 0x10, 0x0a, 0x00, 0x00, 0x06, 0x60, 0x0a, 0x50, 0x12,
                                       0x48, 0x12, 0x48, 0x11, 0x88, 0x10, 0x08, 0x08, 0x10, 0x04,
                                       0x20, 0x02, 0x40, 0x02, 0x40 };
                                                /* a wrench (the user's drawing, §152) */
static const uint8_t mi_system[] = { 0x12, 0x10, 0x0c, 0x00, 0xff, 0x0f, 0x00, 0x30, 0xc0, 0x46,
                                     0x20, 0x40, 0x20, 0x86, 0x10, 0x86, 0x10, 0x86, 0x10, 0x86,
                                     0x10, 0x46, 0x20, 0x40, 0x20, 0x30, 0xc0, 0x0f, 0x00 };

enum { MN_MAIN, MN_USB, MN_SYS, MN_COUNT };
#define M_SUB(m) (-1 - (m))                     /* an action that opens submenu m */
struct mitem {
    const char *name;
    const uint8_t *icon;
    int8_t act;                                 /* M_*, or M_SUB() */
};
static const struct mitem main_items[] = {
    { N_("Files"), mi_files, M_FILES },
    { N_("Now playing"), mi_playing, M_PLAYING },
    { N_("Play queue"), mi_queue, M_QUEUE },
    { N_("Library"), ic_lib, M_LIBRARY },
    /* TRANSLATORS: the main menu's entry for the USB modes (disk, sound card) */
    { N_("USB"), mi_usb, M_SUB(MN_USB) },
    { N_("Settings"), mi_settings, M_SETTINGS },
    /* TRANSLATORS: the main menu's entry for the information, installer and system screens */
    { N_("System"), mi_system, M_SUB(MN_SYS) },
};
static const struct mitem usb_items[] = {
    { N_("Disk, read only"), 0, M_USB_RO },
    { N_("Disk, read/write"), 0, M_USB_RW },
    { N_("Sound card"), 0, M_USB_DAC },
};
static const struct mitem sys_items[] = {
    /* TRANSLATORS: a page of facts: version, formats, free space, battery */
    { N_("Information"), 0, M_INFO },
    { N_("System screen"), 0, M_SYSTEM },
    { N_("Install a firmware (.IMG)"), 0, M_INSTALL },
    { N_("USB loader mode (exit: RST)"), 0, M_LOADER },
};
static const struct {
    const struct mitem *items;
    int n;
    const char *title;
    int8_t from;                                /* its line in the main menu */
} menus[MN_COUNT] = {
    { main_items, 7, N_("Menu"), 0 },
    { usb_items, 3, N_("USB"), 4 },
    { sys_items, 4, N_("System"), 6 },
};

/* A screen of the stack (ui.c, ETUDE §99): VOL+/VOL- move, PLAY chooses (a submenu: opens it),
 * BACK goes up a menu (closes the main one), MENU closes; the choice in menu_choice (-1: none),
 * acted on by ui.c once the menu is off the stack */
int menu_choice = -1;
static int m_menu, m_sel, m_top, m_dirty, m_page;
static int m_tracks, m_images;                  /* the library's tracks, the .IMG files */
static int m_cable;                             /* vbus() as drawn */

static int is_img(const struct fat_dirent *e, void *ctx)
{
    const char *n = e->sname;
    int l = 0;
    while (n[l])
        l++;
    if (!(e->attr & FAT_ATTR_DIR) && l > 4 && n[l - 4] == '.' && (n[l - 3] | 32) == 'i' &&
        (n[l - 2] | 32) == 'm' && (n[l - 1] | 32) == 'g')
        (*(int *)ctx)++;
    return 0;
}

static void menu_enter(void)
{
    m_menu = m_sel = m_top = 0;
    m_dirty = m_page = 1;
    menu_choice = -1;
    m_tracks = lib_status();
    m_cable = vbus();
}

static void menu_shown(void)
{
    m_dirty = m_page = 1;
}

/* Line i of the menu shown: greyed (PLAY does nothing), its text at the right */
static int item_grey(const struct mitem *it)
{
    return (it->act == M_PLAYING && !q_n) || (it->act == M_SUB(MN_USB) && !m_cable) ||
           (it->act == M_INSTALL && !m_images);
}

static void item_info(const struct mitem *it, char *b)
{
    char *p = b, *end = b + 31;
    if (it->act == M_QUEUE && q_n)
        p = log_format(p, end, T("%u tracks"), (uint32_t)q_n);
    else if (it->act == M_LIBRARY)
        p = m_tracks < 0 ? log_format(p, end, "%s", T("not built")) :
            log_format(p, end, T("%u tracks"), (uint32_t)m_tracks);
    else if (it->act == M_SUB(MN_USB) && !m_cable)
        /* TRANSLATORS: why the USB entry is greyed (main menu) */
        p = log_format(p, end, "%s", T("no cable"));
    else if (it->act == M_INSTALL)
        p = m_images ? log_format(p, end, T("%u found"), (uint32_t)m_images) :
            log_format(p, end, "%s", T("none"));
    *p = 0;
}

/* Every pixel written once, nothing cleared first (a clear, then the rows, flickered: §147) */
static void menu_row(int r)
{
    int n = menus[m_menu].n, i = m_top + r, y = ROW_Y0 + 16 * r;
    if (i >= n) {
        lcd_fill(0, y, LCD_W, 16, BG);
        return;
    }
    const struct mitem *it = &menus[m_menu].items[i];
    uint16_t bg = i == m_sel ? SEL_BG : BG;
    int grey = item_grey(it), sub = it->act < 0, xend = list_text_end(n, ROWS), x = 12;
    if (it->icon) {
        lcd_fill(0, y, 3, 16, bg);
        int w = lcd_icon(3, y, it->icon, grey ? DIM : i == m_sel ? FG : DIR_FG, bg);
        lcd_fill(3 + w, y, 22 - 3 - w, 16, bg);
        x = 22;
    } else
        lcd_fill(0, y, x, 16, bg);
    int xmax = xend - 12;                       /* the chevron's column, kept on every line: */
    if (sub && !grey) {                         /* the texts at the right aligned */
        lcd_fill(xmax, y, 2, 16, bg);
        int w = lcd_glyph(xmax + 2, y, '>', i == m_sel ? FG : DIM, bg);
        lcd_fill(xmax + 2 + w, y, xend - xmax - 2 - w, 16, bg);
    } else
        lcd_fill(xmax, y, xend - xmax, 16, bg);
    char b[32];
    item_info(it, b);
    if (b[0]) {
        int x0 = xmax - lcd_text_width(b) - 8;
        text_right(x0, xmax, y, b, DIM, bg);
        xmax = x0;
    }
    lcd_text_clip(x, y, tr(it->name), xmax, grey ? DIM : FG, bg);
}

static int menu_draw(void)
{
    if (vbus() != m_cable) {                    /* the cable in or out: USB (§151) */
        m_cable = vbus();
        m_dirty = 1;
    }
    if (m_page) {
        head_title(tr(menus[m_menu].title), TITLE_FG, 0);
        lcd_fill(0, ROW_Y0 + 16 * ROWS, LCD_W, LCD_H - ROW_Y0 - 16 * ROWS, BG);
        head_status(1);
        m_page = 0;
    } else
        head_status(0);
    if (!m_dirty)
        return SCR_STAY;
    const char *pp = perf_begin("menu draw");
    for (int r = 0; r < ROWS; r++)
        menu_row(r);
    list_scrollbar(ROW_Y0, ROWS, m_top, menus[m_menu].n);
    m_dirty = 0;
    perf_end(pp);
    return SCR_STAY;
}

static void menu_open(int m, int sel)
{
    m_menu = m;
    m_sel = sel;
    m_top = 0;
    m_dirty = m_page = 1;
    if (m == MN_SYS) {
        m_images = 0;                           /* (the root read once: a few sectors) */
        fat_list_dir(0, is_img, &m_images);
    }
}

static int menu_key(const struct key_ev *e)
{
    int ev = e->type, k = e->key, n = menus[m_menu].n;
    if (k == KEY_VOLUP && ev != EV_LONG) {
        list_move(&m_sel, &m_top, n, ROWS, -1);
        m_dirty = 1;
    } else if (k == KEY_VOLDOWN && ev != EV_LONG) {
        list_move(&m_sel, &m_top, n, ROWS, 1);
        m_dirty = 1;
    } else if (k == KEY_BACK && ev == EV_PRESS && m_menu != MN_MAIN) {
        menu_open(MN_MAIN, menus[m_menu].from);
    } else if (k == KEY_BACK || (k == KEY_MENU && ev == EV_PRESS)) {
        menu_choice = -1;
        return SCR_POP;
    } else if (k == KEY_PLAY && ev == EV_PRESS) {
        const struct mitem *it = &menus[m_menu].items[m_sel];
        if (item_grey(it))
            return SCR_STAY;
        if (it->act < 0) {
            menu_open(-1 - it->act, 0);
            return SCR_STAY;
        }
        menu_choice = it->act;
        return SCR_POP;
    }
    return SCR_STAY;
}

const struct screen menu_screen = { menu_enter, menu_shown, menu_key, menu_draw };

/* ---- Information (System, ETUDE §151): what the player is and holds, one line each; any key
 * closes it. Drawn once (the battery's line again when it changes). ---- */
static int i_dirty, i_pct = -1;

static char *size_text(char *p, char *end, uint32_t mib)
{
    if (mib < 1024)
        /* TRANSLATORS: a size (Information) */
        return log_format(p, end, T("%u MB"), mib);
    if (mib < 100 * 1024)
        return log_format(p, end, T("%u.%u GB"), mib / 1024, mib % 1024 * 10 / 1024);
    return log_format(p, end, T("%u GB"), mib / 1024);
}

static void volume_text(char *b, int v)
{
    const struct fat_vol *fv = v == FAT_VOL_SD ? &sdvol : &vol;
    char *p = b, *end = b + 47, f[16], t[16];
    if (!fv->mounted) {
        p = log_format(p, end, "%s", v == FAT_VOL_SD && !sd_present() ? T("no card") :
                                     T("not readable"));
        *p = 0;
        return;
    }
    *size_text(t, t + 15, (uint32_t)((uint64_t)fv->clusters * fv->spc / 2048)) = 0;
    int32_t m = space_free_mib(v);
    if (m < 0)
        p = log_format(p, end, "%s", t);
    else {
        *size_text(f, f + 15, (uint32_t)m) = 0;
        /* TRANSLATORS: the free space and the size of a volume (Information) */
        p = log_format(p, end, T("%s free of %s"), f, t);
    }
    *p = 0;
}

static void info_line(int r, const char *value)  /* a value alone, the whole width */
{
    int y = ROW_Y0 + 16 * r;
    lcd_fill(0, y, 8, 16, BG);
    lcd_text_clip(8, y, value, LCD_W - 8, DIM, BG);
    lcd_fill(LCD_W - 8, y, 8, 16, BG);
}

static void info_row(int r, const char *label, const char *value)
{
    int y = ROW_Y0 + 16 * r, w = lcd_text_width(label) + 16, x0 = 8 + w;
    lcd_fill(0, y, 8, 16, BG);
    lcd_text_clip(8, y, label, x0, FG, BG);
    text_right(x0, LCD_W - 8, y, value, DIM, BG);
    lcd_fill(LCD_W - 8, y, 8, 16, BG);
}

static void info_enter(void)
{
    log_printf("ui: information");
    i_dirty = 1;
    i_pct = -1;
}

static int info_draw(void)
{
    char b[48], *p;
    int pct = (int)battery_shown() | vbus() << 8;
    if (i_dirty) {
        head_title(T("Information"), TITLE_FG, 0);
        head_status(1);
        p = log_format(b, b + 47, EN("%s (%s)"), FW_VERSION, FW_DATE);
        *p = 0;
        /* TRANSLATORS: Information's lines */
        info_row(0, T("Version"), b);
        info_line(1, "WAV AIFF FLAC ALAC MP3 AAC Opus Vorbis MPC");  /* (names) */
        volume_text(b, FAT_VOL_INT);
        info_row(2, T("Internal memory"), b);
        volume_text(b, FAT_VOL_SD);
        info_row(3, T("SD card"), b);
        int t = lib_status();
        p = t < 0 ? log_format(b, b + 47, "%s", T("not built")) :
            log_format(b, b + 47, T("%u tracks"), (uint32_t)t);
        *p = 0;
        info_row(4, T("Library"), b);
        lcd_fill(0, ROW_Y0 + 16 * 7, LCD_W, LCD_H - ROW_Y0 - 16 * 7, BG);
    } else
        head_status(0);
    if (i_dirty || pct != i_pct) {
        unsigned mv = battery_avg_mv();
        p = log_format(b, b + 47, EN("%u.%02u V, %u %%"), mv / 1000, mv % 1000 / 10,
                       (uint32_t)(pct & 0xff));
        *p = 0;
        info_row(5, T("Battery"), b);
        if (vbus())
            p = log_format(b, b + 47, "%s", charging() ? T("charging") : T("charged"));
        else
            p = bat_time_text(b, b + 47, pct & 0xff);
        *p = 0;
        /* TRANSLATORS: the music left on the battery (Information) */
        info_row(6, T("Time left"), b);
        i_pct = pct;
    }
    i_dirty = 0;
    return SCR_STAY;
}

static void info_shown(void)
{
    i_dirty = 1;
}

static int info_key(const struct key_ev *e)
{
    return e->type == EV_PRESS || e->type == EV_LONG ? SCR_POP : SCR_STAY;
}

const struct screen info_screen = { info_enter, info_shown, info_key, info_draw };

/* ---- date and time: VOL+/VOL- change the field, PLAY the next one (the last one saves), BACK
 * the previous one (the first one cancels), MENU cancels. Playback goes on meanwhile. ---- */
static struct rtc_tm c_t;
static int c_ok, c_f, c_dirty;
/* a message shown (millis), for c_msg_ms, then off */
static uint32_t c_msg_at, c_msg_ms;
static const char *c_msg;
static uint16_t c_msg_fg;

static void clock_msg(const char *m, uint16_t fg, uint32_t ms)
{
    c_msg = m;
    c_msg_fg = fg;
    c_msg_ms = ms;
    c_msg_at = millis() | 1;
    c_dirty = 1;
}

static void clock_enter(void)
{
    c_ok = rtc_get(&c_t);
    c_f = 0;
    c_dirty = 1;
    c_msg_at = 0;
    if (c_ok < 0) {
        clock_msg(T("no clock found (I2C1, 0xA2)"), WARN, 1500);
        return;
    }
    if (!c_ok)
        c_t.year = 2026, c_t.mon = 1, c_t.day = 1, c_t.hour = 12, c_t.min = 0;
    c_t.sec = 0;
}

static void clock_shown(void)
{
    c_dirty = 1;
}

static int clock_draw(void)
{
    if (c_msg_at) {                             /* the message, then the screen closes */
        if (c_dirty) {
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(8, 60, c_msg, c_msg_fg, BG);
            c_dirty = 0;
        }
        /* (signed: c_msg_at may be 1 ms ahead, millis() | 1) */
        return (int32_t)(millis() - c_msg_at) >= (int32_t)c_msg_ms ? SCR_POP : SCR_STAY;
    }
    if (!c_dirty)
        return SCR_STAY;
    struct rtc_tm t = c_t;
    int f = c_f, ok = c_ok;
    static const uint8_t ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int dmax = ml[t.mon - 1] + (t.mon == 2 && t.year % 4 == 0);
    if (t.day > dmax)
        t.day = c_t.day = (uint8_t)dmax;
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    lcd_text(4, 0, T("Date and time"), TITLE_FG, BG);
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
    lcd_text(4, 118, T("VOL+/VOL-: change   PLAY: next, save"), DIM, BG);
    lcd_text(4, 136, T("BACK: previous, cancel   MENU: cancel"), DIM, BG);
    if (!ok)
        lcd_text(4, 154, T("the clock had stopped: not set"), WARN, BG);
    c_dirty = 0;
    return SCR_STAY;
}

static int clock_key(const struct key_ev *e)
{
    int ev = e->type, k = e->key;
    if (c_msg_at)
        return SCR_STAY;                        /* the message stays its time */
    struct rtc_tm *t = &c_t;
    if (k == KEY_VOLUP || k == KEY_VOLDOWN) {
        int d = k == KEY_VOLUP ? 1 : -1;
        if (c_f == 0)                           /* 2024 to 2099, round */
            t->year = (uint16_t)(t->year + d < 2024 ? 2099 :
                                 t->year + d > 2099 ? 2024 : t->year + d);
        else if (c_f == 1)
            t->mon = (uint8_t)((t->mon - 1 + 12 + d) % 12 + 1);
        else if (c_f == 2)
            t->day = (uint8_t)((t->day - 1 + 31 + d) % 31 + 1);
        else if (c_f == 3)
            t->hour = (uint8_t)((t->hour + 24 + d) % 24);
        else
            t->min = (uint8_t)((t->min + 60 + d) % 60);
        c_dirty = 1;
    } else if (k == KEY_PLAY && ev == EV_PRESS) {
        if (++c_f == 5) {
            int r = rtc_set(t);
            clock_msg(r ? T("the clock did not answer") : T("clock set"), r ? WARN : FG, 800);
            clock_min = -1;
            return SCR_STAY;
        }
        c_dirty = 1;
    } else if (k == KEY_BACK && ev == EV_PRESS) {
        if (c_f-- == 0)
            return SCR_POP;
        c_dirty = 1;
    } else if (k == KEY_MENU && ev == EV_PRESS)
        return SCR_POP;
    return SCR_STAY;
}

const struct screen clock_screen = { clock_enter, clock_shown, clock_key, clock_draw };
