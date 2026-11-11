/* Firmware log (see log.h). Ring in PMU-SRAM at 0x0309c000 (12 KiB, the free area of the
 * memory plan, kept across resets), then NEWFWLOG.TXT, written in place through the
 * emmc_write_log() fence (the file's own sectors only).
 * The ring holds two kinds of entries: text lines ending with '\n' (log_text), and token
 * records (log_printf, ETUDE §133): 0x01, the time (ms, 4 bytes), the format's offset in the
 * section "logfmt" (2 bytes), the descriptor (4 bytes, log.h), then each argument: 4 bytes, or
 * a string and its 0. They become text when the ring is written into the file, with the formats
 * read from the image on the eMMC (header +0x1e8, tools/mkimg.py). The ring notes which formats
 * its records need (the CRC of the image's formats): the records of a boot before (bound) with
 * the formats of that boot's firmware, then this boot's; a record whose formats are no longer in
 * the image is written raw. */
#include <stdarg.h>
#include "log.h"
#include "board.h"
#include "emmc.h"
#include "fat.h"

#define RING_ADDR   0x0309c000u
#define RING_BYTES  0x3000u
#define RING_MAGIC  0x33474c4eu                 /* "NLG3" (0.103t1: the token records) */
#define DATA_BYTES  (RING_BYTES - 32)
#define FLUSH_US    5000000u                    /* at most every 5 s ... */
#define FLUSH_BYTES 2048u                       /* ... or as soon as this much is pending */
#define REC         0x01                        /* a token record's first byte */
#define REC_HEAD    11
#define REC_MAX     256                         /* a record's bytes at most (strings cut) */
#define ARGS_MAX    24
#define FMT_MAX     256                         /* a format's bytes at most, its 0 included */
#define FMT_MAGIC   0x4d464c4eu                 /* "NLFM", the formats' blob */

struct ring {
    uint32_t magic, boots;
    uint32_t wr, fl;                            /* bytes written / copied to the file, ever */
    uint32_t note;                              /* log_boot_note(): for the next boot */
    uint32_t bound;                             /* where this boot's entries start */
    uint32_t fmt_prev, fmt_cur;                 /* the formats' CRC before bound, from bound */
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
static uint8_t rbyte(uint32_t pos)
{
    return (uint8_t)R->data[pos % DATA_BYTES];
}

static uint32_t rd32(uint32_t pos)
{
    return rbyte(pos) | (uint32_t)rbyte(pos + 1) << 8 | (uint32_t)rbyte(pos + 2) << 16 |
           (uint32_t)rbyte(pos + 3) << 24;
}

/* bytes of the entry at pos (at most avail: a line cut by a reset ends there) */
static uint32_t entry_len(uint32_t pos, uint32_t avail)
{
    uint32_t n = 0;
    if (rbyte(pos) != REC) {
        while (n < avail && rbyte(pos + n++) != '\n')
            ;
        return n;
    }
    if (avail < REC_HEAD)
        return avail;
    uint32_t desc = rd32(pos + 7);
    n = REC_HEAD;
    for (uint32_t i = 0; i < (desc & 31) && n < avail; i++) {
        if (desc >> (5 + i) & 1) {
            while (n < avail && rbyte(pos + n++))
                ;
        } else {
            n += 4;
        }
    }
    return n < avail ? n : avail;
}

static void ring_put(const char *s, uint32_t n)
{
    volatile struct ring *r = R;
    while (r->wr + n - r->fl > DATA_BYTES) {    /* the oldest entries dropped, whole */
        uint32_t k = entry_len(r->fl, r->wr - r->fl);
        logs.lost += k;
        r->fl += k;
    }
    while (n--)
        r->data[r->wr++ % DATA_BYTES] = *s++;
}

static uint32_t log_pending(void)
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

/* the arguments: from a va_list, or from a token record's (strings as pointers) */
struct fargs {
    va_list *ap;
    const uintptr_t *a;
};

static uint32_t arg_u(struct fargs *x)
{
    return x->ap ? va_arg(*x->ap, uint32_t) : (uint32_t)*x->a++;
}

static const char *arg_s(struct fargs *x)
{
    return x->ap ? va_arg(*x->ap, const char *) : (const char *)*x->a++;
}

static char *vformat(char *p, char *end, const char *f, struct fargs *x)
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
            const char *s = arg_s(x);
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
            *p++ = (char)arg_u(x);
            break;
        case 'd': {
            int v = (int)arg_u(x);
            p = put_num(p, end, v < 0 ? -(uint32_t)v : (uint32_t)v, 10, v < 0, width, pad, 0);
            break;
        }
        case 'u':
            p = put_num(p, end, arg_u(x), 10, 0, width, pad, 0);
            break;
        case 'x':
        case 'X':
            p = put_num(p, end, arg_u(x), 16, 0, width, pad, *f == 'X');
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
    struct fargs x = { &ap, 0 };
    p = vformat(p, end, f, &x);
    va_end(ap);
    return p;
}

char *log_format(char *p, char *end, const char *f, ...)
{
    va_list ap;
    va_start(ap, f);
    struct fargs x = { &ap, 0 };
    p = vformat(p, end, f, &x);
    va_end(ap);
    return p;
}

/* One line of text: "[ssss.mmm] text\n" */
static void text_line(const char *fmt, va_list *ap)
{
    char b[256], *p = b, *end = b + sizeof b - 1;
    uint32_t ms = millis();
    struct fargs x = { ap, 0 };

    p = format(p, end, "[%4u.%03u] ", ms / 1000, ms % 1000);
    p = vformat(p, end, fmt, &x);
    while (p > b && p[-1] == '\n')
        p--;
    *p++ = '\n';
    ring_put(b, (uint32_t)(p - b));
}

void log_text(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    text_line(fmt, &ap);
    va_end(ap);
}

#ifndef log_printf
void log_printf(const char *fmt, ...)           /* host builds */
{
    va_list ap;
    va_start(ap, fmt);
    text_line(fmt, &ap);
    va_end(ap);
}
#else
/* A token record (see the top): the format's offset, the arguments as the descriptor says */
void log_tok(const char *fmt, uint32_t desc, ...)
{
    char b[REC_MAX];
    uint32_t ms = millis(), tok = (uint32_t)(fmt - __start_logfmt), n = REC_HEAD;
    va_list ap;

    b[0] = REC;
    for (int i = 0; i < 4; i++)
        b[1 + i] = (char)(ms >> 8 * i);
    b[5] = (char)tok;
    b[6] = (char)(tok >> 8);
    for (int i = 0; i < 4; i++)
        b[7 + i] = (char)(desc >> 8 * i);
    va_start(ap, desc);
    for (uint32_t i = 0; i < (desc & 31); i++) {
        /* a string's bytes: what leaves its 0 and 4 bytes to each argument after it (from
         * n + 4 x the arguments left <= REC_MAX, true at the start: 11 + 4 x 24) */
        int room = REC_MAX - (int)n - 4 * (int)((desc & 31) - i - 1) - 1;
        if (desc >> (5 + i) & 1) {
            const char *s = va_arg(ap, const char *);
            for (int k = 0; s && s[k] && k < room; k++)
                b[n++] = s[k];
            b[n++] = 0;
        } else {
            uint32_t v = va_arg(ap, uint32_t);
            for (int k = 0; k < 4; k++)
                b[n++] = (char)(v >> 8 * k);
        }
    }
    va_end(ap);
    ring_put(b, n);
}
#endif

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
    r->fmt_prev = logs.recovered ? r->fmt_cur : 0;  /* the boot before's records: its formats */
    r->bound = r->wr;
    r->fmt_cur = 0;                             /* this boot's: known at log_attach() */
    logs.boots = ++r->boots;
    logs.attached = 0;
}

/* ---- the formats, in the image (FW1, else FW2): header +0x1e8 = {offset, length} of
 * "NLFM", version 1, CRC, length, then the section "logfmt" ---- */
#if !defined(log_printf)                        /* host builds without tokens: none */
static uint32_t fmt_check(void)
{
    return 0;
}

static int fmt_read(uint32_t tok, char *out, uint32_t n)
{
    (void)tok, (void)out, (void)n;
    return -1;
}
#elif defined(LOG_TOKENS_HOST)                  /* log_test: the section in memory */
extern uint32_t log_host_fmt;                   /* the formats' CRC the test says */

static uint32_t fmt_check(void)
{
    return log_host_fmt;
}

static int fmt_read(uint32_t tok, char *out, uint32_t n)
{
    const char *f = __start_logfmt + tok;
    for (uint32_t i = 0; i < n; i++)
        if (!(out[i] = f[i]))
            return 0;
    return -1;
}
#else
static uint32_t fmt_lba, fmt_len, fmt_sec_lba[2], fmt_next;
static uint8_t fmt_sec[2][512] __attribute__((aligned(4)));

/* the CRC of the formats in the image now (0: none), the sector cache emptied */
static uint32_t fmt_check(void)
{
    uint32_t *h = (uint32_t *)(void *)fmt_sec[0];
    fmt_sec_lba[0] = fmt_sec_lba[1] = 0;
    fmt_len = 0;
    for (int k = 0; k < 2 && emmc.p; k++) {
        uint32_t lba, len;
        if (fw_blob(k, 0x1e8, FMT_MAGIC, h, &lba, &len) || len < 16 || h[1] != 1 ||
            h[3] > len - 16)
            continue;
        fmt_lba = lba;
        fmt_len = h[3];
        return h[2];
    }
    return 0;
}

/* the format at offset tok of the section, into out (n bytes with its 0): 0, or -1 */
static int fmt_read(uint32_t tok, char *out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++, tok++) {
        if (tok >= fmt_len)
            return -1;
        uint32_t off = 16 + tok, lba = fmt_lba + off / 512, j = 0;
        while (j < 2 && fmt_sec_lba[j] != lba)
            j++;
        if (j == 2) {
            j = fmt_next++ & 1;
            fmt_sec_lba[j] = 0;
            if (emmc_read(lba, 1, fmt_sec[j]))
                return -1;
            fmt_sec_lba[j] = lba;
        }
        if (!(out[i] = (char)fmt_sec[j][off % 512]))
            return 0;
    }
    return -1;
}
#endif

/* The entry at pos (len bytes) as text into [p, end): a line as it is; a record made from its
 * format when the image holds the formats it was written with, else raw */
static char *entry_text(uint32_t pos, uint32_t len, char *p, char *end, uint32_t now_id)
{
    if (rbyte(pos) != REC) {
        for (uint32_t i = 0; i < len && p < end; i++)
            *p++ = (char)rbyte(pos + i);
        return p;
    }
    static char strs[REC_MAX];                  /* the strings, one after another */
    static char f[FMT_MAX];
    uintptr_t a[ARGS_MAX] = { 0 };
    uint32_t sn = 0;
    uint32_t ms = 0, tok, desc, n = REC_HEAD;
    char *line = p;
    for (int i = 0; i < 4; i++)
        ms |= (uint32_t)rbyte(pos + 1 + i) << 8 * i;
    tok = rbyte(pos + 5) | (uint32_t)rbyte(pos + 6) << 8;
    desc = rd32(pos + 7);
    uint32_t na = desc & 31;
    if (na > ARGS_MAX)
        na = ARGS_MAX;
    for (uint32_t i = 0; i < na; i++) {
        if (desc >> (5 + i) & 1) {
            a[i] = (uintptr_t)(strs + sn);
            while (n < len && rbyte(pos + n)) {
                if (sn < REC_MAX - 1)
                    strs[sn++] = (char)rbyte(pos + n);
                n++;
            }
            strs[sn < REC_MAX ? sn++ : REC_MAX - 1] = 0;
            n++;
        } else {
            a[i] = rd32(pos + n);
            n += 4;
        }
    }
    uint32_t id = (int32_t)(pos - R->bound) < 0 ? R->fmt_prev : R->fmt_cur;
    p = format(p, end, "[%4u.%03u] ", ms / 1000, ms % 1000);
    if (id && id == now_id && !fmt_read(tok, f, sizeof f)) {
        struct fargs x = { 0, a };
        p = vformat(p, end, f, &x);
        while (p > line && p[-1] == '\n')
            p--;
    } else {
        p = format(p, end, "?log %04x", tok);   /* readlog.py --elf makes it text */
        for (uint32_t i = 0; i < na; i++)
            p = desc >> (5 + i) & 1 ? format(p, end, " \"%s\"", (const char *)a[i])
                                    : format(p, end, " %x", (uint32_t)a[i]);
    }
    *p++ = '\n';                                /* (end leaves room for it) */
    return p;
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

static uint32_t state_sector, crash_sector;

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
    logs.file_sectors = count - 2;              /* the last two: the fault record (0.99r32), */
    crash_sector = first + count - 2;           /* then the resume state */
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
        if (next >= 1 && next < logs.file_sectors && fl < 512) {
            logs.next = next;
            fill = fl;
            logs.wraps = parse_hex(h + 40, 8);
            if (fill && emmc_read(first + next, 1, cur))
                return logs.error = -5;
            for (uint32_t i = fill; i < 512; i++)
                cur[i] = ' ';
        }
    }
    if (!R->fmt_cur)                            /* this boot's records: these formats */
        R->fmt_cur = fmt_check();
    logs.error = 0;
    logs.attached = 1;
    last_flush = ticks();
    return 0;
}

static int write_cur(void)
{
    if (!emmc_write_log(file_first + logs.next, 1, cur))
        return 0;
    logs.attached = 0;
    return logs.error = -6;
}

/* The ring's entries as text into the file, entry by entry */
int log_flush(void)
{
    volatile struct ring *r = R;
    static char line[256];                      /* a line at most, as log_text() */
    int wrote = 0, dirty = 0;

    if (!logs.attached)
        return -1;
    last_flush = ticks();
    if (r->fl == r->wr)
        return 0;
    uint32_t now_id = fmt_check();
    while (r->fl != r->wr) {
        uint32_t len = entry_len(r->fl, r->wr - r->fl);
        char *e = entry_text(r->fl, len, line, line + sizeof line - 1, now_id);
        uint32_t n = (uint32_t)(e - line);
        for (uint32_t i = 0; i < n;) {
            uint32_t take = 512 - fill < n - i ? 512 - fill : n - i;
            for (uint32_t k = 0; k < take; k++)
                cur[fill + k] = (uint8_t)line[i + k];
            fill += take;
            i += take;
            dirty = 1;
            if (fill == 512) {
                if (write_cur())
                    return logs.error;
                wrote = 1;
                dirty = 0;
                fill = 0;
                for (int k = 0; k < 512; k++)
                    cur[k] = ' ';
                if (++logs.next >= logs.file_sectors) {
                    logs.next = 1;
                    logs.wraps++;
                }
            }
        }
        r->fl += len;
    }
    if (dirty) {
        if (write_cur())
            return logs.error;
        wrote = 1;
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

/* ---- the fault record (crash.c): the sector before the resume state ---- */
int log_crash_read(void *buf)
{
    if (!logs.attached || !crash_sector)
        return -1;
    return emmc_read(crash_sector, 1, buf);
}

int log_crash_write(const void *buf)
{
    if (!logs.attached || !crash_sector)
        return -1;
    return emmc_write_log(crash_sector, 1, buf);
}
