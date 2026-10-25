/* Firmware log (see log.h). Ring in PMU-SRAM at 0x0309c000 (12 KiB, the free area of the
 * memory plan, kept across resets), then NEWFWLOG.TXT, written in place through the
 * emmc_write_log() fence (the file's own sectors only). */
#include <stdarg.h>
#include "log.h"
#include "board.h"
#include "emmc.h"
#include "fat.h"

#define RING_ADDR   0x0309c000u
#define RING_BYTES  0x3000u
#define RING_MAGIC  0x32474c4eu                 /* "NLG2" (0.68: the note word added) */
#define DATA_BYTES  (RING_BYTES - 20)
#define FLUSH_US    5000000u                    /* at most every 5 s ... */
#define FLUSH_BYTES 2048u                       /* ... or as soon as this much is pending */

struct ring {
    uint32_t magic, boots;
    uint32_t wr, fl;                            /* bytes written / copied to the file, ever */
    uint32_t note;                              /* log_boot_note(): for the next boot */
    char data[DATA_BYTES];
};

#ifndef LOG_RING                                /* host tests put it elsewhere */
#define LOG_RING RING_ADDR
#endif
#define R ((volatile struct ring *)LOG_RING)

struct log_state logs;

static uint32_t file_first, fill, last_flush;
static uint8_t cur[512];                        /* sector being filled */
static uint8_t hdr[512];

/* ---- ring ---- */
static void ring_put(const char *s, uint32_t n)
{
    volatile struct ring *r = R;
    while (n--)
        r->data[r->wr++ % DATA_BYTES] = *s++;
    if (r->wr - r->fl > DATA_BYTES) {           /* oldest bytes overwritten */
        logs.lost += r->wr - r->fl - DATA_BYTES;
        r->fl = r->wr - DATA_BYTES;
    }
}

uint32_t log_pending(void)
{
    return R->wr - R->fl;
}

/* ---- formatting: %s %c %d %u %x %X, width and 0 flag; %% ---- */
static char *put_num(char *p, char *end, uint32_t v, int base, int neg, int width, char pad,
                     int upper)
{
    char t[12];
    int n = 0;
    do {
        int d = v % base;
        t[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= base;
    } while (v);
    if (neg)
        t[n++] = '-';
    while (width-- > n && p < end)
        *p++ = pad;
    while (n && p < end)
        *p++ = t[--n];
    return p;
}

static char *vformat(char *p, char *end, const char *f, va_list ap)
{
    while (*f && p < end) {
        if (*f != '%') {
            *p++ = *f++;
            continue;
        }
        f++;
        char pad = ' ';
        int width = 0;
        if (*f == '0')
            pad = '0', f++;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (*f++ - '0');
        while (*f == 'l')
            f++;
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0;
            while (s && s[n])
                n++;
            while (width-- > n && p < end)
                *p++ = ' ';
            while (s && *s && p < end)
                *p++ = *s++;
            break;
        }
        case 'c':
            *p++ = (char)va_arg(ap, int);
            break;
        case 'd': {
            int v = va_arg(ap, int);
            p = put_num(p, end, v < 0 ? -(uint32_t)v : (uint32_t)v, 10, v < 0, width, pad, 0);
            break;
        }
        case 'u':
            p = put_num(p, end, va_arg(ap, uint32_t), 10, 0, width, pad, 0);
            break;
        case 'x':
        case 'X':
            p = put_num(p, end, va_arg(ap, uint32_t), 16, 0, width, pad, *f == 'X');
            break;
        case '%':
            *p++ = '%';
            break;
        default:
            if (!*f)
                return p;
            *p++ = '?';
        }
        f++;
    }
    return p;
}

static char *format(char *p, char *end, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    p = vformat(p, end, f, ap);
    va_end(ap);
    return p;
}

/* One line: "[ssss.mmm] text\n" */
void log_printf(const char *fmt, ...)
{
    char b[256], *p = b, *end = b + sizeof b - 1;
    uint32_t ms = millis();
    va_list ap;

    p = format(p, end, "[%4u.%03u] ", ms / 1000, ms % 1000);
    va_start(ap, fmt);
    p = vformat(p, end, fmt, ap);
    va_end(ap);
    while (p > b && p[-1] == '\n')
        p--;
    *p++ = '\n';
    ring_put(b, (uint32_t)(p - b));
}

void log_init(void)
{
    volatile struct ring *r = R;

    if (r->magic != RING_MAGIC || r->wr - r->fl > DATA_BYTES) {
        r->magic = RING_MAGIC;
        r->boots = 0;
        r->wr = r->fl = 0;
        r->note = 0;
    }
    logs.recovered = r->wr - r->fl;
    logs.boots = ++r->boots;
    logs.attached = 0;
}

void log_boot_note(uint32_t v)
{
    R->note = v;
}

uint32_t log_boot_note_take(void)
{
    uint32_t v = R->note;
    R->note = 0;
    return v;
}

/* ---- file ---- */
static uint32_t parse_hex(const uint8_t *p, int n)
{
    uint32_t v = 0;
    while (n--) {
        uint8_t c = *p++;
        v = v << 4 | (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

/* "NEWFWLOG 1 next=XXXXXXXX fill=XXX wraps=XXXXXXXX boots=XXXXXXXX" (hex), then spaces */
static int write_header(void)
{
    char *p = (char *)hdr, *end = p + 511;
    for (int i = 0; i < 512; i++)
        hdr[i] = ' ';
    format(p, end, "NEWFWLOG 1 next=%08x fill=%03x wraps=%08x boots=%08x "
           "(circular text log of newfw; read it with tools/readlog.py)",
           logs.next, fill, logs.wraps, logs.boots);
    for (int i = 0; i < 511; i++)
        if (!hdr[i])
            hdr[i] = ' ';
    hdr[511] = '\n';
    return emmc_write_log(file_first, 1, hdr);
}

static uint32_t state_sector;

int log_attach(void)
{
    struct fat_file f;
    uint32_t first, count;

    logs.attached = 0;
    if (fat_find_root(LOG_FILE, &f))
        return logs.error = -1;                 /* no file */
    if (fat_extent(&f, &first, &count))
        return logs.error = -2;                 /* fragmented */
    if (count > f.size / 512)
        count = f.size / 512;                   /* only whole sectors inside the file size */
    if (count < 16 || count > 32768)
        return logs.error = -3;                 /* 8 KiB to 16 MiB */
    if (emmc_log_area(first, count))
        return logs.error = -4;
    file_first = first;
    logs.file_sectors = count - 1;              /* the last sector keeps the resume state */
    state_sector = first + count - 1;
    if (emmc_read(first, 1, hdr))
        return logs.error = -5;
    logs.next = 1;
    logs.wraps = 0;
    fill = 0;
    for (int i = 0; i < 512; i++)
        cur[i] = ' ';
    const uint8_t *h = hdr;
    if (h[0] == 'N' && h[1] == 'E' && h[2] == 'W' && h[3] == 'F' && h[4] == 'W' && h[9] == '1' &&
        h[11] == 'n' && h[15] == '=' && h[29] == '=' && h[39] == '=') {
        uint32_t next = parse_hex(h + 16, 8), fl = parse_hex(h + 30, 3);
        if (next >= 1 && next < count - 1 && fl < 512) {
            logs.next = next;
            fill = fl;
            logs.wraps = parse_hex(h + 40, 8);
            if (fill && emmc_read(first + next, 1, cur))
                return logs.error = -5;
            for (uint32_t i = fill; i < 512; i++)
                cur[i] = ' ';
        }
    }
    logs.error = 0;
    logs.attached = 1;
    last_flush = ticks();
    return 0;
}

int log_flush(void)
{
    volatile struct ring *r = R;
    int wrote = 0;

    if (!logs.attached)
        return -1;
    last_flush = ticks();
    while (r->fl != r->wr) {
        uint32_t take = 512 - fill, avail = r->wr - r->fl;
        if (take > avail)
            take = avail;
        for (uint32_t i = 0; i < take; i++)
            cur[fill + i] = (uint8_t)r->data[(r->fl + i) % DATA_BYTES];
        if (emmc_write_log(file_first + logs.next, 1, cur)) {
            for (uint32_t i = fill; i < 512; i++)
                cur[i] = ' ';
            logs.attached = 0;
            return logs.error = -6;
        }
        wrote = 1;
        r->fl += take;
        fill += take;
        if (fill == 512) {
            fill = 0;
            for (int i = 0; i < 512; i++)
                cur[i] = ' ';
            if (++logs.next >= logs.file_sectors) {
                logs.next = 1;
                logs.wraps++;
            }
        }
    }
    if (wrote && write_header()) {
        logs.attached = 0;
        return logs.error = -6;
    }
    return 0;
}

void log_poll(void)
{
    uint32_t n = log_pending();
    if (logs.attached && n && (n >= FLUSH_BYTES || ticks() - last_flush > FLUSH_US))
        log_flush();
}

void log_detach(void)
{
    log_flush();
    logs.attached = 0;
    emmc_log_area(0, 0);                        /* closes the fence */
}

/* ---- the resume state of the user interface: the last sector of the log file ---- */
int log_state_read(void *buf)
{
    if (!logs.attached || !state_sector)
        return -1;
    return emmc_read(state_sector, 1, buf);
}

int log_state_write(const void *buf)
{
    if (!logs.attached || !state_sector)
        return -1;
    return emmc_write_log(state_sector, 1, buf);
}
