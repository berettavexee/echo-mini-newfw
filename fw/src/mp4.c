/* MP4 audio (M4A) for the player (ETUDE §55): the container is read here, on core A, and the
 * audio frames go to core B as a packet stream (hifi_proto.h: a header, the decoder
 * configuration, then each frame with its length). The sample tables stay in the file and are
 * read as they are needed through 512-byte windows, one per table (as the stock firmware does,
 * ETUDE §54): sizes (stsz), chunk offsets (stco/co64), samples per chunk (stsc). The frames
 * themselves are copied through a 4 KiB window, so that the frames of a chunk, contiguous in the
 * file, cost one read. Only the first audio track is played; AAC-LC only (ALAC: later).
 * Gapless: the encoder delay and padding from iTunSMPB (iTunes), else from the edit list. */
#include "mp4.h"
#include "hifi_proto.h"
#include "log.h"

void *memcpy(void *d, const void *s, unsigned n);

static const struct fat_file *file;
static struct fat_cursor box_cur;

static uint32_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (b) << 16 | (c) << 8 | (d))

/* Small reads of the boxes, through a one-sector cache (the walk reads 8-byte headers one
 * after the other: one sector read each without it) */
static uint8_t sec[512] __attribute__((aligned(4)));
static uint32_t sec_pos = 0xffffffffu, sec_len;

static int rd(uint32_t pos, void *buf, uint32_t n)
{
    uint8_t *d = buf;
    if (pos > file->size || n > file->size - pos)
        return -1;
    while (n) {
        uint32_t base = pos & ~511u;
        if (base != sec_pos) {
            uint32_t m = file->size - base < 512 ? file->size - base : 512;
            if (fat_pread(file, &box_cur, base, sec, m) != (int)m)
                return -1;
            sec_pos = base;
            sec_len = m;
        }
        uint32_t k = sec_len - (pos - base) < n ? sec_len - (pos - base) : n;
        memcpy(d, sec + (pos - base), k);
        d += k;
        pos += k;
        n -= k;
    }
    return 0;
}

/* ---- boxes ---- */
struct box { uint32_t type, start, body, end; };     /* start of the box, of its body, end */

/* The box at pos inside [.., end): 0, or -1 when there is none or it does not fit */
static int box_at(uint32_t pos, uint32_t end, struct box *b)
{
    uint8_t h[16];
    if (pos + 8 > end || rd(pos, h, 8))
        return -1;
    uint32_t size = be32(h);
    b->type = be32(h + 4);
    b->start = pos;
    b->body = pos + 8;
    if (size == 1) {                            /* 64-bit size: only below 4 GiB here */
        if (rd(pos + 8, h + 8, 8) || be32(h + 8))
            return -1;
        size = be32(h + 12);
        b->body = pos + 16;
    } else if (size == 0)
        size = end - pos;                       /* up to the end of the parent */
    if (size < b->body - pos || size > end - pos)
        return -1;
    b->end = pos + size;
    return 0;
}

/* The first child of type 'type' in [from, end) */
static int find(uint32_t from, uint32_t end, uint32_t type, struct box *b)
{
    for (uint32_t pos = from; !box_at(pos, end, b); pos = b->end)
        if (b->type == type)
            return 0;
    return -1;
}

/* ---- tables read through windows ---- */
struct table {
    uint32_t off, count, esz;           /* first entry in the file, entries, bytes per entry */
    uint32_t win;                       /* file offset of buf[0] */
    uint32_t len;
    struct fat_cursor c;
    uint8_t buf[512 + 12];              /* a sector, and an entry across its end */
};
static struct table stsz, stco, stsc;
static uint32_t stsz_fixed;             /* stsz: one size for every frame (0: the table) */
static uint32_t reads;

static const uint8_t *entry(struct table *t, uint32_t i)
{
    uint32_t pos = t->off + i * t->esz;
    if (i >= t->count)
        return 0;
    if (pos < t->win || pos + t->esz > t->win + t->len) {
        uint32_t base = pos & ~511u, n = file->size - base < 512 ? file->size - base : 512;
        if (pos + t->esz > base + n)
            n = pos + t->esz - base;            /* at most 512 + 12: entries are small */
        if (n > sizeof t->buf || fat_pread(file, &t->c, base, t->buf, n) != (int)n) {
            log_printf("mp4: table entry %u at %u not readable (window %u, %u bytes)", i, pos, base, n);
            return 0;
        }
        t->win = base;
        t->len = n;
        reads++;
    }
    return t->buf + (pos - t->win);
}

static int table_init(struct table *t, const struct box *b, uint32_t esz, uint32_t skip)
{
    uint8_t h[8];
    if (rd(b->body, h, 8))
        return -1;
    t->count = be32(h + 4);
    t->off = b->body + 8 + skip;
    t->esz = esz;
    t->win = t->len = 0;
    t->c.index = t->c.cluster = 0;
    return (uint64_t)t->count * esz > b->end - t->off ? -1 : 0;
}

/* ---- the packet stream ---- */
static uint8_t head[sizeof(struct hp_pkt_header) + 64] __attribute__((aligned(4)));
static uint32_t head_len, head_done;
static uint32_t smp, total;             /* the next frame, frames in the track */
static uint32_t chunk, chunk_first, chunk_n;    /* chunk of 'smp', its first frame, frames in it */
static uint32_t stsc_i, stsc_next;      /* stsc entry in use, first chunk (1-based) of the next */
static uint32_t pos, size;              /* file offset and size of frame 'smp' */
static uint32_t done;                   /* bytes of frame 'smp' written (length bytes included) */
static uint8_t win[4096];
static uint32_t win_pos, win_len;
static struct fat_cursor win_cur;
static int failed;

static uint32_t sample_size(uint32_t i)
{
    const uint8_t *e;
    if (stsz_fixed)
        return stsz_fixed;
    if (!(e = entry(&stsz, i)))
        failed = 1;
    return e ? be32(e) : 0;
}

static uint32_t chunk_offset(uint32_t c)
{
    const uint8_t *e = entry(&stco, c);
    if (!e)
        failed = 1;
    return !e ? 0 : stco.esz == 8 ? (be32(e) ? 0xffffffffu : be32(e + 4)) : be32(e);
}

/* stsc entry i: frames per chunk; and the first chunk (1-based) of entry i + 1 */
static uint32_t stsc_entry(uint32_t i, uint32_t *next_first)
{
    const uint8_t *e = entry(&stsc, i), *n = entry(&stsc, i + 1);
    if (!e)
        failed = 1;
    *next_first = n ? be32(n) : 0xffffffffu;
    return e ? be32(e + 4) : 1;
}

/* Frame 'smp' found from scratch: the stsc runs walked up to it, then the sizes before it in its
 * chunk added to the chunk offset */
static void locate(uint32_t k)
{
    uint32_t first = 0, next;                   /* first frame of the run */
    stsc_i = 0;
    for (;;) {
        const uint8_t *e = entry(&stsc, stsc_i);
        uint32_t spc = stsc_entry(stsc_i, &next), fc = e ? be32(e) : 1;
        uint32_t chunks = next == 0xffffffffu ? stco.count - (fc - 1) : next - fc;
        if (failed || !spc)
            return;
        if (next == 0xffffffffu || k < first + chunks * spc) {
            uint32_t ci = (k - first) / spc;
            chunk = fc - 1 + ci;
            chunk_first = first + ci * spc;
            chunk_n = spc;
            stsc_next = next;
            break;
        }
        first += chunks * spc;
        stsc_i++;
    }
    pos = chunk_offset(chunk);
    for (uint32_t i = chunk_first; i < k; i++)
        pos += sample_size(i);
    smp = k;
    size = sample_size(k);
}

static void next_sample(void)
{
    smp++;
    done = 0;
    if (smp >= total)
        return;
    if (smp - chunk_first >= chunk_n) {         /* the next chunk */
        chunk++;
        chunk_first = smp;
        if (chunk + 1 == stsc_next) {
            stsc_i++;
            chunk_n = stsc_entry(stsc_i, &stsc_next);
        }
        pos = chunk_offset(chunk);
    } else
        pos += size;
    size = sample_size(smp);
}

/* Bytes of the stream into the ring at stream offset wr, wrapping at its end */
static void ring_put(uint32_t wr, const void *src, uint32_t n)
{
    uint32_t o = wr % HP_IN_SIZE, k = HP_IN_SIZE - o < n ? HP_IN_SIZE - o : n;
    memcpy((void *)(HP_IN_BASE + o), src, k);
    if (n > k)
        memcpy((void *)HP_IN_BASE, (const uint8_t *)src + k, n - k);
}

uint32_t mp4_feed(uint32_t wr, uint32_t room, uint32_t *nreads)
{
    uint32_t w0 = wr;
    reads = 0;
    if (head_done < head_len) {
        uint32_t k = head_len - head_done < room ? head_len - head_done : room;
        ring_put(wr, head + head_done, k);
        head_done += k;
        wr += k;
        room -= k;
    }
    while (room && smp < total && !failed) {
        if (done < 2) {                         /* the length, little-endian */
            uint8_t l[2] = { (uint8_t)size, (uint8_t)(size >> 8) };
            if (size > 0xffff) {
                failed = 1;
                break;
            }
            uint32_t k = 2 - done < room ? 2 - done : room;
            ring_put(wr, l + done, k);
            done += k;
            wr += k;
            room -= k;
            continue;
        }
        uint32_t at = pos + done - 2, left = size - (done - 2);
        if (!left) {
            next_sample();
            continue;
        }
        if (at < win_pos || at >= win_pos + win_len) {
            if (at >= file->size) {
                failed = 1;
                break;
            }
            uint32_t base = at & ~511u, n = sizeof win;
            if (n > file->size - base)
                n = file->size - base;
            if (fat_pread(file, &win_cur, base, win, n) != (int)n) {
                log_printf("mp4: frame %u at %u not readable", smp, at);
                failed = 1;
                break;
            }
            win_pos = base;
            win_len = n;
            reads++;
        }
        uint32_t k = win_pos + win_len - at;
        if (!k) {                               /* the frame goes past the end of the file */
            failed = 1;
            break;
        }
        if (k > left)
            k = left;
        if (k > room)
            k = room;
        ring_put(wr, win + (at - win_pos), k);
        done += k;
        wr += k;
        room -= k;
    }
    if (failed && smp < total) {
        log_printf("mp4: frame %u of %u not readable: the stream ends there", smp, total);
        total = smp;
    }
    *nreads = reads;
    return wr - w0;
}

int mp4_done(void)
{
    return head_done == head_len && smp >= total;
}

/* ---- the container ---- */
/* iTunSMPB: " 00000000 DDDDDDDD PPPPPPPP LLLLLLLLLLLLLLLL ..." (hex: delay, padding, length) */
static int itunsmpb(const uint8_t *s, uint32_t n, struct mp4_track *t)
{
    uint32_t v[4] = { 0 }, f = 0, i = 0;
    uint64_t len = 0;
    while (i < n && f < 4) {
        while (i < n && s[i] == ' ')
            i++;
        if (i >= n)
            break;
        uint64_t x = 0;
        for (; i < n && s[i] != ' '; i++) {
            uint32_t c = s[i];
            uint32_t d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ?
                         (c | 32) - 'a' + 10 : 16;
            if (d > 15)
                return -1;
            x = x << 4 | d;
        }
        if (f == 3)
            len = x;
        else
            v[f] = (uint32_t)x;
        f++;
    }
    if (f < 4 || !len || len > 0xffffffffu)
        return -1;
    t->delay = v[1];
    t->padding = v[2];
    t->length = (uint32_t)len;
    return 0;
}

/* moov/udta/meta/ilst/---- named iTunSMPB */
static int find_itunsmpb(const struct box *moov, struct mp4_track *t)
{
    struct box udta, meta, ilst, item, sub;
    if (find(moov->body, moov->end, FOURCC('u', 'd', 't', 'a'), &udta) ||
        find(udta.body, udta.end, FOURCC('m', 'e', 't', 'a'), &meta) ||
        find(meta.body + 4, meta.end, FOURCC('i', 'l', 's', 't'), &ilst))
        return -1;
    for (uint32_t p = ilst.body; !box_at(p, ilst.end, &item); p = item.end) {
        if (item.type != FOURCC('-', '-', '-', '-'))
            continue;
        int named = 0;
        for (uint32_t q = item.body; !box_at(q, item.end, &sub); q = sub.end) {
            uint8_t s[192];                     /* iTunSMPB: 8 + 116 bytes */
            uint32_t n = sub.end - sub.body;
            if (sub.type == FOURCC('n', 'a', 'm', 'e') && n == 4 + 8 &&
                !rd(sub.body, s, n) && !__builtin_memcmp(s + 4, "iTunSMPB", 8))
                named = 1;
            else if (named && sub.type == FOURCC('d', 'a', 't', 'a') && n > 8 && n <= sizeof s &&
                     !rd(sub.body, s, n))
                return itunsmpb(s + 8, n - 8, t);
        }
    }
    return -1;
}

/* The AudioSpecificConfig in esds: ES descriptor (3), decoder config (4), decoder specific (5) */
static int esds_config(const struct box *esds, uint8_t *cfg, uint32_t *cfg_len)
{
    uint8_t d[128];
    uint32_t n = esds->end - esds->body, i = 4;     /* after version and flags */
    if (n > sizeof d || rd(esds->body, d, n))
        return -1;
    while (i + 2 <= n) {
        uint32_t tag = d[i++], len = 0;
        for (int k = 0; k < 4 && i < n; k++) {
            uint32_t b = d[i++];
            len = len << 7 | (b & 0x7f);
            if (!(b & 0x80))
                break;
        }
        if (tag == 3) {                         /* ES_ID, flags, then optional fields */
            uint32_t fl = i + 2 < n ? d[i + 2] : 0;
            i += 3 + (fl & 0x80 ? 2 : 0);       /* dependsOn_ES_ID */
            if (fl & 0x40)
                i += 1 + (i < n ? d[i] : 0);    /* URL */
            if (fl & 0x20)
                i += 2;                         /* OCR_ES_Id */
        } else if (tag == 4) {
            if (i < n && d[i] != 0x40)          /* not MPEG-4 audio */
                return -1;
            i += 13;
        } else if (tag == 5) {
            if (len > 64 || i + len > n)
                return -1;
            memcpy(cfg, d + i, len);
            *cfg_len = len;
            return 0;
        } else
            i += len;
    }
    return -1;
}

static const uint32_t aac_rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                        16000, 12000, 11025, 8000, 7350 };

int mp4_open(const struct fat_file *f, uint32_t seek_ms, struct mp4_track *t)
{
    struct box moov, trak, mdia, b, minf, stbl, stsd, entry_box, esds;
    struct hp_pkt_header *h = (struct hp_pkt_header *)head;
    uint8_t buf[36];
    file = f;
    box_cur.index = box_cur.cluster = win_cur.index = win_cur.cluster = 0;
    sec_pos = 0xffffffffu;
    __builtin_memset(t, 0, sizeof *t);
    head_len = head_done = 0;
    smp = total = done = 0;
    win_pos = win_len = 0;
    failed = 0;
    if (find(0, f->size, FOURCC('m', 'o', 'o', 'v'), &moov))
        return MP4_EFORMAT;
    uint32_t ts = 0, dur = 0;
    for (uint32_t p = moov.body;; p = trak.end) {   /* the first sound track */
        if (find(p, moov.end, FOURCC('t', 'r', 'a', 'k'), &trak))
            return MP4_EFORMAT;
        if (find(trak.body, trak.end, FOURCC('m', 'd', 'i', 'a'), &mdia) ||
            find(mdia.body, mdia.end, FOURCC('h', 'd', 'l', 'r'), &b) || rd(b.body, buf, 12))
            continue;
        if (be32(buf + 8) == FOURCC('s', 'o', 'u', 'n'))
            break;
    }
    if (!find(mdia.body, mdia.end, FOURCC('m', 'd', 'h', 'd'), &b) && !rd(b.body, buf, 32)) {
        int v1 = buf[0] == 1;
        ts = be32(buf + (v1 ? 20 : 12));
        dur = v1 ? (be32(buf + 24) ? 0xffffffffu : be32(buf + 28)) : be32(buf + 16);
    }
    if (find(mdia.body, mdia.end, FOURCC('m', 'i', 'n', 'f'), &minf) ||
        find(minf.body, minf.end, FOURCC('s', 't', 'b', 'l'), &stbl) ||
        find(stbl.body, stbl.end, FOURCC('s', 't', 's', 'd'), &stsd) ||
        box_at(stsd.body + 8, stsd.end, &entry_box))
        return MP4_EFORMAT;
    if (entry_box.type == FOURCC('a', 'l', 'a', 'c'))
        return MP4_EALAC;
    if (entry_box.type != FOURCC('m', 'p', '4', 'a') || rd(entry_box.body, buf, 28))
        return MP4_EUNSUPPORTED;
    /* AudioSampleEntry: QuickTime versions 1 and 2 have 16 or 36 more bytes before the children */
    uint32_t qt = be16(buf + 8), kids = entry_box.body + 28 + (qt == 1 ? 16 : qt == 2 ? 36 : 0);
    if (find(kids, entry_box.end, FOURCC('e', 's', 'd', 's'), &esds)) {
        struct box wave;                        /* QuickTime: esds inside 'wave' */
        if (find(kids, entry_box.end, FOURCC('w', 'a', 'v', 'e'), &wave) ||
            find(wave.body, wave.end, FOURCC('e', 's', 'd', 's'), &esds))
            return MP4_EFORMAT;
    }
    uint8_t *cfg = head + sizeof *h;
    uint32_t cfg_len = 0;
    if (esds_config(&esds, cfg, &cfg_len) || cfg_len < 2)
        return MP4_EFORMAT;
    uint32_t aot = cfg[0] >> 3, sri = (cfg[0] & 7) << 1 | cfg[1] >> 7, ch = cfg[1] >> 3 & 15;
    if (aot != 2 || sri > 12 || ch < 1 || ch > 2 || (cfg[1] & 4)) {
        log_printf("mp4: audio object type %u, rate index %u, %u channels%s: not played (AAC-LC "
                   "only, mono or stereo, 1024-sample frames)", aot, sri, ch,
                   cfg[1] & 4 ? ", 960-sample frames" : "");
        return MP4_EUNSUPPORTED;
    }
    t->rate = aac_rates[sri];
    t->channels = ch;

    /* tables */
    if (find(stbl.body, stbl.end, FOURCC('s', 't', 's', 'z'), &b) || rd(b.body, buf, 12))
        return MP4_EFORMAT;
    stsz_fixed = be32(buf + 4);
    stsz.count = be32(buf + 8);
    stsz.off = b.body + 12;
    stsz.esz = 4;
    stsz.win = stsz.len = 0;
    stsz.c.index = stsz.c.cluster = 0;
    if (!stsz_fixed && (uint64_t)stsz.count * 4 > b.end - stsz.off)
        return MP4_EFORMAT;
    if (find(stbl.body, stbl.end, FOURCC('s', 't', 's', 'c'), &b) || table_init(&stsc, &b, 12, 0) ||
        !stsc.count)
        return MP4_EFORMAT;
    if (!find(stbl.body, stbl.end, FOURCC('s', 't', 'c', 'o'), &b)) {
        if (table_init(&stco, &b, 4, 0))
            return MP4_EFORMAT;
    } else if (find(stbl.body, stbl.end, FOURCC('c', 'o', '6', '4'), &b) || table_init(&stco, &b, 8, 0))
        return MP4_EFORMAT;
    total = t->samples = stsz.count;
    if (!total || !stco.count)
        return MP4_EFORMAT;

    /* length, delay and padding: iTunSMPB, else the edit list, else the whole track */
    uint32_t decoded = total > 0xffffffffu / 1024 ? 0xffffffffu : total * 1024;
    if (find_itunsmpb(&moov, t) || t->delay + t->length > decoded) {
        struct box edts, elst, mvhd;
        uint32_t movie_ts = 0;
        t->delay = t->padding = 0;
        t->length = decoded;
        if (!find(moov.body, moov.end, FOURCC('m', 'v', 'h', 'd'), &mvhd) && !rd(mvhd.body, buf, 24))
            movie_ts = be32(buf + (buf[0] == 1 ? 20 : 12));
        if (!find(trak.body, trak.end, FOURCC('e', 'd', 't', 's'), &edts) &&
            !find(edts.body, edts.end, FOURCC('e', 'l', 's', 't'), &elst) && !rd(elst.body, buf, 20) &&
            buf[0] == 0 && be32(buf + 4) >= 1 && ts && movie_ts) {
            uint32_t seg = be32(buf + 8), mt = be32(buf + 12);   /* the first entry */
            if (mt != 0xffffffffu) {
                uint64_t d = (uint64_t)mt * t->rate / ts, l = (uint64_t)seg * t->rate / movie_ts;
                if (d + l <= decoded && l) {
                    t->delay = (uint32_t)d;
                    t->length = (uint32_t)l;
                    t->padding = decoded - t->delay - t->length;
                }
            }
        }
    }
    (void)dur;

    /* where the stream starts: a frame before the target (the overlap of the transform) */
    uint32_t t0 = seek_ms ? (uint32_t)((uint64_t)seek_ms * t->rate / 1000) : 0;
    if (t0 >= t->length)
        t0 = 0;
    uint32_t d0 = t0 + t->delay, k = d0 / 1024, start = seek_ms && k ? k - 1 : 0;
    if (start >= total)
        start = total - 1;
    h->magic = HP_PKT_MAGIC;
    h->rate = t->rate;
    h->channels = t->channels;
    h->skip = d0 - start * 1024;
    h->length = t->length;
    h->start_frame = t0;
    h->config_len = cfg_len;
    head_len = sizeof *h + cfg_len;
    locate(start);
    if (failed)
        return MP4_EREAD;
    log_printf("mp4: AAC-LC %u Hz, %u ch, %u frames of 1024, delay %u, padding %u, length %u; "
               "start at frame %u (skip %u)", t->rate, t->channels, total, t->delay, t->padding,
               t->length, start, h->skip);
    return MP4_OK;
}
