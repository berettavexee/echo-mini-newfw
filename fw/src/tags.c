/* Tags for the play screen (step 6, ETUDE §25): title, artist, album, track number, read by core A
 * when a track starts. ID3v2.2/2.3/2.4 (then ID3v1) for MP3, Vorbis comments for FLAC, Ogg
 * Vorbis and Opus (read through the Ogg pages), RIFF INFO or an ID3v2 chunk for WAV, ID3v2 or NAME
 * and AUTH chunks for AIFF (0.74, ETUDE §53; both after the samples too). Big fields (cover art, lyrics)
 * are skipped by position, never read. Unsynchronised or compressed ID3v2 frames are skipped.
 * ReplayGain (0.55, ETUDE §35): REPLAYGAIN_TRACK/ALBUM_GAIN/PEAK in Vorbis comments and ID3v2
 * TXXX frames; R128_TRACK/ALBUM_GAIN in Opus (Q7.8 dB for -23 LUFS: +5 dB for the -18 LUFS of
 * ReplayGain; the header's output gain is applied by the decoder). */
#include <stdint.h>
#include "bytes.h"
#include "fat.h"
#include "hifi_proto.h"
#include "tags.h"
#include "text.h"

#define SCAN_MAX (512 * 1024)                   /* never look further into a file (past the */
                                                /* samples of WAV and AIFF: further than them) */
#define SKIP_MAX (16u << 20)                    /* ... nor, with the parts skipped, past 16 MiB */

/* ---- reading the file ---- */
static struct {
    const struct fat_file *f;
    struct fat_cursor c;
    uint32_t pos;                               /* file offset of buf[0] */
    uint32_t len;
    uint32_t chunk;                             /* bytes read per miss: 2 KiB, 16 KiB for Ogg */
    uint32_t lim;                               /* SCAN_MAX, or that much past the samples */
    uint8_t buf[16384] __attribute__((aligned(4)));
} rd;

/* 'chunk' bytes from a sector boundary: whole aligned sectors, which fat_pread() reads in one
 * eMMC command per run of clusters (each command costs a fixed delay, about 0.5 ms on the
 * device). The Ogg comment header is crossed page by page: 16 KiB at a time. */
static int rd_at(uint32_t pos, uint32_t n, const uint8_t **p)
{
    if (n > sizeof rd.buf - 512 || pos >= rd.f->size || pos + n > rd.f->size || pos + n > rd.lim)
        return -1;
    if (pos < rd.pos || pos + n > rd.pos + rd.len) {
        uint32_t base = pos & ~511u, m = rd.chunk;
        if (pos + n > base + m)
            m = sizeof rd.buf;
        if (m > rd.f->size - base)
            m = rd.f->size - base;
        if (fat_pread(rd.f, &rd.c, base, rd.buf, m) != (int)m)
            return -1;
        rd.pos = base;
        rd.len = m;
    }
    *p = rd.buf + (pos - rd.pos);
    return 0;
}

/* n bytes skipped by their declared size (cover art), never read: the window goes on that much
 * further, up to SKIP_MAX (fields after a big picture) */
static void skipped(uint32_t n)
{
    if (rd.lim < SKIP_MAX)
        rd.lim = n < SKIP_MAX - rd.lim ? rd.lim + n : SKIP_MAX;
}

static uint32_t syncsafe(const uint8_t *p) { return p[0] << 21 | p[1] << 14 | p[2] << 7 | p[3]; }

/* ---- text into a field: UTF-8 out, cut on a character boundary ---- */
/* enc: 0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16 BE, 3 UTF-8 */
static void set_text(char *d, int max, const uint8_t *s, uint32_t n, int enc)
{
    int o = 0, be = enc == 2;
    uint32_t i = 0;
    if (enc == 1 && n >= 2) {
        be = s[0] == 0xfe && s[1] == 0xff;
        i = 2;
    }
    while (i < n) {
        uint32_t c;
        if (enc == 0)
            c = s[i++];
        else if (enc == 3) {
            if (!s[i])
                break;
            if (o < max - 1)
                d[o++] = (char)s[i];
            i++;
            continue;
        } else {
            if (i + 1 >= n)
                break;
            c = be ? (uint32_t)s[i] << 8 | s[i + 1] : s[i] | (uint32_t)s[i + 1] << 8;
            i += 2;
            if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) {
                uint32_t c2 = be ? (uint32_t)s[i] << 8 | s[i + 1] : s[i] | (uint32_t)s[i + 1] << 8;
                if (c2 >= 0xdc00 && c2 < 0xe000) {
                    c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
                    i += 2;
                }
            }
        }
        if (!c)
            break;
        utf8_put(d, max, &o, c);
    }
    /* UTF-8 cut in the middle of a character (even right after its lead byte): drop it */
    if (o > 0 && enc == 3 && ((uint8_t)d[o - 1] & 0x80)) {
        int k = o - 1;
        while (k > 0 && ((uint8_t)d[k] & 0xc0) == 0x80)
            k--;
        int need = ((uint8_t)d[k] & 0xe0) == 0xc0 ? 2 : ((uint8_t)d[k] & 0xf0) == 0xe0 ? 3 : 4;
        if (o - k < need)
            o = k;
    }
    while (o > 0 && d[o - 1] == ' ')
        o--;
    d[o] = 0;
}

static uint16_t num(const char *s)
{
    uint16_t v = 0;
    while (*s >= '0' && *s <= '9')
        v = (uint16_t)(v * 10 + (*s++ - '0'));
    return v;
}

static int key_is(const uint8_t *s, uint32_t n, const char *key)  /* "KEY=", no case */
{
    uint32_t i = 0;
    for (; key[i]; i++) {
        uint8_t c = i < n ? s[i] : 0;
        if (c >= 'a' && c <= 'z')
            c = (uint8_t)(c - 32);
        if (c != (uint8_t)key[i])
            return 0;
    }
    return i < n && s[i] == '=';
}

/* ---- ReplayGain ---- */
/* "-6.54 dB" -> -654 (1/100 dB); 1 when it does not read as a number */
static int rg_parse_db(const char *s, int32_t *out)
{
    while (*s == ' ')
        s++;
    int neg = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    if (*s < '0' || *s > '9')
        return 1;
    int32_t v = 0;
    while (*s >= '0' && *s <= '9' && v < 100000)
        v = v * 10 + (*s++ - '0');
    v *= 100;
    if (*s == '.' || *s == ',') {
        s++;
        if (*s >= '0' && *s <= '9') {
            v += (*s++ - '0') * 10;
            if (*s >= '0' && *s <= '9')
                v += *s - '0';
        }
    }
    if (v > 9900)
        return 1;
    *out = neg ? -v : v;
    return 0;
}

/* "0.988525" -> Q16 */
static uint32_t rg_parse_peak(const char *s)
{
    while (*s == ' ')
        s++;
    uint32_t ip = 0, fr = 0, scale = 1;
    while (*s >= '0' && *s <= '9' && ip < 1000)
        ip = ip * 10 + (uint32_t)(*s++ - '0');
    if (*s == '.' || *s == ',')
        for (s++; *s >= '0' && *s <= '9' && scale < 1000000; s++) {
            fr = fr * 10 + (uint32_t)(*s - '0');
            scale *= 10;
        }
    if (ip >= 1000)
        return 0;
    return ip * 65536 + (uint32_t)((uint64_t)fr * 65536 / scale);
}

/* key: the field name in capitals, value: its text */
static void rg_field(struct tags *t, const char *key, const char *value)
{
    static const char *const names[] = { "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_ALBUM_GAIN",
                                         "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_PEAK",
                                         "R128_TRACK_GAIN", "R128_ALBUM_GAIN" };
    int i = 0;
    for (; i < 6; i++) {
        const char *a = key, *b = names[i];
        while (*a && *a == *b)
            a++, b++;
        if (*a == *b)
            break;
    }
    int32_t v;
    if (i < 2) {
        if (!rg_parse_db(value, &v) && t->rg_gain[i] == RG_NONE)
            t->rg_gain[i] = (int16_t)v;
    } else if (i < 4)
        t->rg_peak[i - 2] = rg_parse_peak(value);
    else if (i < 6) {                           /* signed Q7.8, -23 LUFS: before REPLAYGAIN_* */
        const char *s = value;
        int neg = *s == '-';
        if (neg || *s == '+')
            s++;
        v = 0;
        while (*s >= '0' && *s <= '9' && v < 65536)
            v = v * 10 + (*s++ - '0');
        if (v < 32768 && s != value) {
            v = (neg ? -v : v) * 100 / 256 + 500;
            t->rg_gain[i - 4] = (int16_t)v;
        }
    }
}

/* A Vorbis comment or a TXXX description: its name in capitals (rg_field matches it) */
static int rg_key(const uint8_t *s, uint32_t n, char *key, int max)
{
    int k = 0;
    for (uint32_t i = 0; i < n && s[i] && s[i] != '='; i++) {
        if (k >= max - 1)
            return 0;
        uint8_t c = s[i];
        key[k++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    key[k] = 0;
    return k > 0;
}

/* One Vorbis comment "KEY=value" (UTF-8) */
static void vorbis_comment(struct tags *t, const uint8_t *s, uint32_t n)
{
    if (key_is(s, n, "TITLE"))
        set_text(t->title, sizeof t->title, s + 6, n - 6, 3);
    else if (key_is(s, n, "ARTIST"))
        set_text(t->artist, sizeof t->artist, s + 7, n - 7, 3);
    else if (key_is(s, n, "ALBUM"))
        set_text(t->album, sizeof t->album, s + 6, n - 6, 3);
    else if (key_is(s, n, "TRACKNUMBER")) {
        char v[8];
        set_text(v, sizeof v, s + 12, n - 12, 3);
        t->track = num(v);
    } else if ((s[0] | 0x20) == 'r') {
        char key[24], v[24];
        uint32_t k = 0;
        while (k < n && s[k] != '=')
            k++;
        if (k < n && rg_key(s, k, key, sizeof key)) {
            set_text(v, sizeof v, s + k + 1, n - k - 1, 3);
            rg_field(t, key, v);
        }
    }
}

/* An ID3v2 TXXX frame: encoding, description, value */
static void id3_txxx(struct tags *t, const uint8_t *d, uint32_t n)
{
    int enc = d[0];
    uint32_t i = 1, w = enc == 1 || enc == 2 ? 2 : 1;
    while (i + w <= n && (d[i] || (w == 2 && d[i + 1])))
        i += w;
    if (i + w > n)
        return;
    char key[24], v[24];
    set_text(key, sizeof key, d + 1, i - 1, enc);
    for (int k = 0; key[k]; k++)
        if (key[k] >= 'a' && key[k] <= 'z')
            key[k] = (char)(key[k] - 32);
    if (!rg_key((const uint8_t *)key, sizeof key, key, sizeof key))
        return;
    set_text(v, sizeof v, d + i + w, n - i - w, enc);
    rg_field(t, key, v);
}

/* ---- ID3 ---- */
/* An ID3v2 tag at 'base': the start of an MP3, or a chunk of WAV and AIFF */
static uint32_t id3v2(struct tags *t, uint32_t base)
{
    const uint8_t *h;
    if (rd_at(base, 10, &h) || h[0] != 'I' || h[1] != 'D' || h[2] != '3' || h[3] < 2 || h[3] > 4)
        return 0;
    int ver = h[3], flags = h[5];
    uint32_t end = 10 + syncsafe(h + 6), pos = 10;
    if (flags & 0x80)
        return end;                             /* whole-tag unsynchronisation: not read */
    if (ver >= 3 && (flags & 0x40)) {           /* extended header */
        const uint8_t *x;
        if (rd_at(base + 10, 4, &x))
            return end;
        pos += ver == 4 ? syncsafe(x) : 4 + be32(x);
    }
    int hl = ver == 2 ? 6 : 10;
    while (pos + hl <= end) {
        const uint8_t *fh;
        if (rd_at(base + pos, (uint32_t)hl, &fh) || !fh[0])
            break;
        uint32_t size = ver == 2 ? (uint32_t)fh[3] << 16 | fh[4] << 8 | fh[5] :
                        ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        int fflags = ver == 2 ? 0 : fh[9];
        const char *id = (const char *)fh;
        char *dst = 0;
        int max = 0, trk = 0;
        if (ver == 2) {
            if (id[0] == 'T' && id[1] == 'T' && id[2] == '2') { dst = t->title; max = sizeof t->title; }
            else if (id[0] == 'T' && id[1] == 'P' && id[2] == '1') { dst = t->artist; max = sizeof t->artist; }
            else if (id[0] == 'T' && id[1] == 'A' && id[2] == 'L') { dst = t->album; max = sizeof t->album; }
            else if (id[0] == 'T' && id[1] == 'R' && id[2] == 'K') trk = 1;
        } else {
            if (!__builtin_memcmp(id, "TIT2", 4)) { dst = t->title; max = sizeof t->title; }
            else if (!__builtin_memcmp(id, "TPE1", 4)) { dst = t->artist; max = sizeof t->artist; }
            else if (!__builtin_memcmp(id, "TALB", 4)) { dst = t->album; max = sizeof t->album; }
            else if (!__builtin_memcmp(id, "TRCK", 4)) trk = 1;
            else if (!__builtin_memcmp(id, "TXXX", 4)) trk = 2;
        }
        /* compressed, encrypted or unsynchronised frames are skipped */
        if (!(dst || trk) || size < 2 || (fflags & (ver == 4 ? 0x0e : 0xc0)))
            skipped(size);
        else {
            uint32_t n = size > 1024 ? 1024 : size;
            const uint8_t *d;
            if (!rd_at(base + pos + hl, n, &d)) {
                if (trk == 2)
                    id3_txxx(t, d, n);
                else if (trk) {
                    char v[8];
                    set_text(v, sizeof v, d + 1, n - 1, d[0]);
                    t->track = num(v);
                } else
                    set_text(dst, max, d + 1, n - 1, d[0]);
            }
        }
        pos += hl + size;                       /* cover art and the rest: skipped */
    }
    return end;
}

static void id3v1(struct tags *t)
{
    const uint8_t *p;
    uint32_t sz = rd.f->size;
    if (sz < 128 || sz - 128 >= SCAN_MAX) {
        /* the end of a big file: read it on its own, the window limit does not apply */
        if (sz < 128)
            return;
        if (fat_pread(rd.f, &rd.c, sz - 128, rd.buf, 128) != 128)
            return;
        rd.len = 0;                             /* the cache now holds the tail only */
        p = rd.buf;
    } else if (rd_at(sz - 128, 128, &p))
        return;
    if (p[0] != 'T' || p[1] != 'A' || p[2] != 'G')
        return;
    if (!t->title[0])
        set_text(t->title, sizeof t->title, p + 3, 30, 0);
    if (!t->artist[0])
        set_text(t->artist, sizeof t->artist, p + 33, 30, 0);
    if (!t->album[0])
        set_text(t->album, sizeof t->album, p + 63, 30, 0);
    if (!t->track && !p[125] && p[126])
        t->track = p[126];
}

/* ---- FLAC ---- */
static void flac(struct tags *t)
{
    const uint8_t *p;
    if (rd_at(0, 4, &p) || p[0] != 'f' || p[1] != 'L' || p[2] != 'a' || p[3] != 'C')
        return;
    uint32_t pos = 4;
    for (int last = 0; !last;) {
        if (rd_at(pos, 4, &p))
            return;
        last = p[0] & 0x80;
        int type = p[0] & 0x7f;
        uint32_t len = (uint32_t)p[1] << 16 | p[2] << 8 | p[3], body = pos + 4;
        if (type == 4) {                        /* VORBIS_COMMENT */
            uint32_t q = body;
            if (rd_at(q, 4, &p))
                return;
            q += 4 + le32(p);                   /* vendor */
            if (rd_at(q, 4, &p))
                return;
            uint32_t count = le32(p);
            q += 4;
            for (uint32_t i = 0; i < count && q < body + len; i++) {
                if (rd_at(q, 4, &p))
                    return;
                uint32_t n = le32(p);
                if (n > 512)
                    skipped(n);
                else if (!rd_at(q + 4, n, &p))
                    vorbis_comment(t, p, n);
                q += 4 + n;
            }
            return;
        }
        pos = body + len;                       /* cover art: skipped */
        skipped(len);
    }
}

/* ---- Ogg: the comment packet, read as a byte stream through the pages ---- */
static struct { uint32_t page, seg_left, data, page_end; int nsegs, seg; uint8_t lace[255]; } og;

static int og_page(uint32_t pos)
{
    const uint8_t *h;
    if (rd_at(pos, 27, &h) || h[0] != 'O' || h[1] != 'g' || h[2] != 'g' || h[3] != 'S')
        return -1;
    og.nsegs = h[26];
    if (rd_at(pos + 27, (uint32_t)og.nsegs, &h))
        return -1;
    for (int i = 0; i < og.nsegs; i++)
        og.lace[i] = h[i];
    og.page = pos;
    og.data = pos + 27 + og.nsegs;
    og.seg = 0;
    uint32_t body = 0;
    for (int i = 0; i < og.nsegs; i++)
        body += og.lace[i];
    og.page_end = og.data + body;
    og.seg_left = 0;
    return 0;
}

/* The next n bytes of the stream (within the current packet's pages) into out (or skipped) */
static int og_read(uint8_t *out, uint32_t n)
{
    while (n) {
        if (og.data >= og.page_end) {
            if (og_page(og.page_end))
                return -1;
            continue;
        }
        uint32_t k = og.page_end - og.data;
        if (k > n)
            k = n;
        if (out) {
            uint32_t m = k > 2048 ? 2048 : k;
            const uint8_t *p;
            if (rd_at(og.data, m, &p))
                return -1;
            for (uint32_t i = 0; i < m; i++)
                out[i] = p[i];
            out += m;
            k = m;
        } else
            skipped(k);
        og.data += k;
        n -= k;
    }
    return 0;
}

static uint32_t og_u32(void)
{
    uint8_t b[4];
    return og_read(b, 4) ? 0xffffffffu : le32(b);
}

/* After the last field: the comment packet ends with the first segment shorter than 255 bytes
 * that ends at or after the current position. Its page and the index of the next segment go to
 * core B, which then skips the whole comment header (cover art) without reading it. */
static void og_packet_end(struct tags *t)
{
    for (int pages = 0; pages < 64; pages++) {
        uint32_t end = og.page + 27 + (uint32_t)og.nsegs;
        for (int s = 0; s < og.nsegs; s++) {
            end += og.lace[s];
            if (end >= og.data && og.lace[s] < 255) {
                t->ogg_end_page = og.page;
                t->ogg_end_seg = (uint32_t)s + 1;
                og.data = end;
                return;
            }
        }
        if (og_page(og.page_end))
            return;
    }
}

static void ogg(struct tags *t)
{
    /* page 1: identification; the comment packet starts on the next page */
    if (og_page(0))
        return;
    if (og_page(og.page_end))
        return;
    uint8_t hd[8];
    if (og_read(hd, 8))
        return;
    int opus = !__builtin_memcmp(hd, "OpusTags", 8);
    if (!opus) {
        if (hd[0] != 3 || __builtin_memcmp(hd + 1, "vorbis", 6))
            return;
        og.data -= 1;                           /* "\3vorbis" is 7 bytes: one back */
    }
    uint32_t vendor = og_u32();
    if (vendor > 4096 || og_read(0, vendor))
        return;
    uint32_t count = og_u32();
    static uint8_t c[512];
    if (count > 4096)
        return;
    for (uint32_t i = 0; i < count; i++) {     /* all of them: the end goes to core B */
        uint32_t n = og_u32();
        if (n == 0xffffffffu)
            return;
        if (n <= sizeof c) {
            if (og_read(c, n))
                return;
            vorbis_comment(t, c, n);
        } else if (og_read(0, n))               /* cover art (METADATA_BLOCK_PICTURE): skipped */
            return;
    }
    og_packet_end(t);
    if (!opus && t->ogg_end_page) {             /* check: the setup header follows */
        uint8_t s[7];
        if (og_read(s, 7) || s[0] != 5 || __builtin_memcmp(s + 1, "vorbis", 6))
            t->ogg_end_page = t->ogg_end_seg = 0;
    }
}

/* ---- WAV: LIST INFO ---- */
/* The tags may follow the samples: the window goes on past them */
static void past_samples(uint32_t end)
{
    rd.lim = end < 0xffffffffu - SCAN_MAX ? end + SCAN_MAX : 0xffffffffu;
}

static void wav(struct tags *t)
{
    const uint8_t *p;
    if (rd_at(0, 12, &p) || __builtin_memcmp(p, "RIFF", 4) || __builtin_memcmp(p + 8, "WAVE", 4))
        return;
    for (uint32_t pos = 12; pos + 8 <= rd.f->size && pos < rd.lim;) {
        if (rd_at(pos, 12, &p))
            return;
        uint32_t len = le32(p + 4), next = pos + 8 + len + (len & 1);
        if (!__builtin_memcmp(p, "LIST", 4) && !__builtin_memcmp(p + 8, "INFO", 4)) {
            for (uint32_t q = pos + 12; q + 8 <= pos + 8 + len;) {
                const uint8_t *s;
                if (rd_at(q, 8, &s))
                    return;
                uint32_t n = le32(s + 4);
                char *dst = 0;
                int max = 0;
                if (!__builtin_memcmp(s, "INAM", 4)) { dst = t->title; max = sizeof t->title; }
                else if (!__builtin_memcmp(s, "IART", 4)) { dst = t->artist; max = sizeof t->artist; }
                else if (!__builtin_memcmp(s, "IPRD", 4)) { dst = t->album; max = sizeof t->album; }
                if (dst && n <= 512 && !rd_at(q + 8, n, &s))
                    set_text(dst, max, s, n, 3);
                q += 8 + n + (n & 1);
            }
        } else if (!__builtin_memcmp(p, "id3 ", 4) || !__builtin_memcmp(p, "ID3 ", 4))
            id3v2(t, pos + 8);
        else if (!__builtin_memcmp(p, "data", 4))
            past_samples(next);
        if (next <= pos)
            return;
        pos = next;
    }
}

/* AIFF and AIFF-C: chunk sizes big-endian; an "ID3 " chunk (iTunes, ffmpeg), else NAME / AUTH */
static void aiff(struct tags *t)
{
    const uint8_t *p;
    if (rd_at(0, 12, &p) || __builtin_memcmp(p, "FORM", 4) ||
        (__builtin_memcmp(p + 8, "AIFF", 4) && __builtin_memcmp(p + 8, "AIFC", 4)))
        return;
    for (uint32_t pos = 12; pos + 8 <= rd.f->size && pos < rd.lim;) {
        if (rd_at(pos, 8, &p))
            return;
        uint32_t len = be32(p + 4), next = pos + 8 + len + (len & 1);
        int name = !__builtin_memcmp(p, "NAME", 4), auth = !__builtin_memcmp(p, "AUTH", 4);
        char *dst = name && !t->title[0] ? t->title : auth && !t->artist[0] ? t->artist : 0;
        if (dst && len <= 512 && !rd_at(pos + 8, len, &p))
            set_text(dst, name ? (int)sizeof t->title : (int)sizeof t->artist, p, len, 0);
        else if (!__builtin_memcmp(p, "ID3 ", 4) || !__builtin_memcmp(p, "id3 ", 4))
            id3v2(t, pos + 8);
        else if (!__builtin_memcmp(p, "SSND", 4))
            past_samples(next);
        if (next <= pos)
            return;
        pos = next;
    }
}

/* MP4 (M4A, 0.75, ETUDE §55): moov/udta/meta/ilst; each item holds a 'data' box (type, locale,
 * then the value): ©nam, ©ART, ©alb, trkn, and ---- items named replaygain_* (beets, foobar2000) */
static int mp4_box(uint32_t pos, uint32_t end, const uint8_t **id, uint32_t *body, uint32_t *next)
{
    const uint8_t *p;
    if (pos + 8 > end || rd_at(pos, 16 < end - pos ? 16 : 8, &p))
        return -1;
    uint32_t size = be32(p);
    *id = p + 4;
    *body = pos + 8;
    if (size == 1) {
        if (end - pos < 16 || be32(p + 8))
            return -1;
        size = be32(p + 12);
        *body = pos + 16;
    } else if (!size)
        size = end - pos;
    if (size < *body - pos || size > end - pos)
        return -1;
    *next = pos + size;
    return 0;
}

static int mp4_find(uint32_t from, uint32_t end, const char *type, uint32_t *body, uint32_t *bend)
{
    const uint8_t *id;
    for (uint32_t pos = from, next; !mp4_box(pos, end, &id, body, &next); pos = next) {
        if (!__builtin_memcmp(id, type, 4)) {
            *bend = next;
            return 0;
        }
        if (!__builtin_memcmp(id, "mdat", 4))
            past_samples(next);                 /* moov after the samples (ffmpeg) */
    }
    return -1;
}

static void mp4(struct tags *t)
{
    uint32_t b, e, end = rd.f->size;
    if (mp4_find(0, end, "moov", &b, &e) || mp4_find(b, e, "udta", &b, &e) ||
        mp4_find(b, e, "meta", &b, &e) || mp4_find(b + 4, e, "ilst", &b, &e))
        return;
    const uint8_t *id, *p;
    for (uint32_t pos = b, ib, next; !mp4_box(pos, e, &id, &ib, &next); pos = next) {
        char *dst = 0;
        int max = 0;
        uint32_t c = be32(id), db, de;
        if (c == 0xa96e616d) { dst = t->title; max = sizeof t->title; }         /* ©nam */
        else if (c == 0xa9415254) { dst = t->artist; max = sizeof t->artist; }  /* ©ART */
        else if (c == 0xa9616c62) { dst = t->album; max = sizeof t->album; }    /* ©alb */
        else if (c != 0x74726b6e && c != 0x2d2d2d2d)                            /* trkn, ---- */
            continue;
        char key[32];
        key[0] = 0;
        if (c == 0x2d2d2d2d) {                  /* ----: mean, name, data */
            uint32_t nb, ne;
            if (mp4_find(ib, next, "name", &nb, &ne) || ne - nb <= 4 || ne - nb > 4 + 31 ||
                rd_at(nb + 4, ne - nb - 4, &p) || !rg_key(p, ne - nb - 4, key, sizeof key))
                continue;
        }
        if (mp4_find(ib, next, "data", &db, &de) || de - db < 8 || de - db > 8 + 512 ||
            rd_at(db, de - db, &p))
            continue;
        uint32_t n = de - db - 8;
        if (dst)
            set_text(dst, max, p + 8, n, 3);
        else if (key[0]) {
            char v[32];
            set_text(v, sizeof v, p + 8, n, 3);
            rg_field(t, key, v);
        } else if (n >= 4)
            t->track = be16(p + 10);
    }
}

/* APEv2 (0.80, ETUDE §61: Musepack; also WavPack, APE): a footer "APETAGEX" in the last 32
 * bytes (or before an ID3v1 tag), the items before it: value length, flags, key, value (UTF-8).
 * Title, Artist, Album, Track ("3/12"), REPLAYGAIN_* */
static void apev2(struct tags *t)
{
    uint32_t sz = rd.f->size;
    const uint8_t *p;
    rd.lim = 0xffffffffu;                       /* at the end of the file */
    for (int pass = 0; pass < 2; pass++) {
        uint32_t end = sz - (pass ? 128 : 0);   /* the footer, or before ID3v1 */
        if (end < 32 || rd_at(end - 32, 32, &p) || __builtin_memcmp(p, "APETAGEX", 8))
            continue;
        uint32_t size = le32(p + 12), n = le32(p + 16);
        if (size < 32 || size > end || size > SKIP_MAX)
            return;
        /* item by item: its header and key, then its value when small (cover art: skipped) */
        uint32_t pos = end - size, stop = end - 32;
        for (uint32_t i = 0; i < n && pos + 8 < stop; i++) {
            uint32_t h = stop - pos > 264 ? 264 : stop - pos;  /* sizes, flags, a key of 255 */
            if (rd_at(pos, h, &p))
                return;
            uint32_t vlen = le32(p), k = 8;
            while (k < h && p[k])
                k++;                            /* the key, up to its 0 */
            if (k >= h || vlen > stop - pos - k - 1)
                return;
            char kk[32];
            int ok = rg_key(p + 8, k - 8, kk, sizeof kk);
            uint32_t v = pos + k + 1;
            pos = v + vlen;
            if (!ok || vlen > 512 || rd_at(v, vlen, &p))
                continue;
            if (!__builtin_memcmp(kk, "TITLE", 6))
                set_text(t->title, sizeof t->title, p, vlen, 3);
            else if (!__builtin_memcmp(kk, "ARTIST", 7))
                set_text(t->artist, sizeof t->artist, p, vlen, 3);
            else if (!__builtin_memcmp(kk, "ALBUM", 6))
                set_text(t->album, sizeof t->album, p, vlen, 3);
            else if (!__builtin_memcmp(kk, "TRACK", 6)) {
                char tv[8];
                set_text(tv, sizeof tv, p, vlen, 3);
                t->track = num(tv);
            } else {
                char val[32];
                set_text(val, sizeof val, p, vlen, 3);
                rg_field(t, kk, val);
            }
        }
        return;
    }
}

void tags_read(const struct fat_file *f, uint32_t codec, struct tags *t)
{
    t->title[0] = t->artist[0] = t->album[0] = 0;
    t->track = 0;
    t->rg_gain[0] = t->rg_gain[1] = RG_NONE;
    t->rg_peak[0] = t->rg_peak[1] = 0;
    t->ogg_end_page = t->ogg_end_seg = 0;
    rd.chunk = codec == HP_CODEC_VORBIS || codec == HP_CODEC_OPUS ? sizeof rd.buf : 2048;
    rd.f = f;
    rd.lim = SCAN_MAX;
    rd.c.index = rd.c.cluster = 0;
    rd.pos = rd.len = 0;
    if (codec == HP_CODEC_MP3) {
        id3v2(t, 0);
        if (!t->title[0] || !t->artist[0])
            id3v1(t);
    } else if (codec == HP_CODEC_FLAC)
        flac(t);
    else if (codec == HP_CODEC_VORBIS || codec == HP_CODEC_OPUS)
        ogg(t);
    else if (codec == HP_CODEC_WAV)
        wav(t);
    else if (codec == HP_CODEC_AIFF)
        aiff(t);
    else if (codec == HP_CODEC_AAC || codec == HP_CODEC_ALAC)
        mp4(t);
    else if (codec == HP_CODEC_MPC)
        apev2(t);
}
