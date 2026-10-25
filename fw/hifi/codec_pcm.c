/* Uncompressed PCM for our core B runtime (ETUDE §53): WAV (RIFF, PCM or WAVE_FORMAT_EXTENSIBLE)
 * and AIFF / AIFF-C (FORM; compression "NONE" or "twos" big-endian, "sowt" little-endian), told
 * apart by their first four bytes: one image for HP_CODEC_WAV and HP_CODEC_AIFF. 8/16/24/32-bit
 * integer (AIFF: any size up to 32 bits, left-justified in whole bytes), mono or stereo. 16-bit
 * sources go to the ring as packed 16-bit frames (4 bytes), the others as 32-bit frames (8 bytes,
 * data in the high bits); mono is duplicated. AIFF needs COMM before SSND (as every encoder
 * writes it): the input only moves forward. */
#include "rt.h"

const uint32_t codec_id = HP_CODEC_WAV;

static uint32_t data_left, block;           /* bytes of sample data left, bytes per frame */
static int big;                             /* samples big-endian (AIFF) */
static int signed8;                         /* 8-bit samples signed (AIFF), else offset (WAV) */

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* AIFF's sample rate: an 80-bit IEEE extended number (sign and 15-bit exponent, then a 64-bit
 * mantissa with its integer bit); 0 when out of range */
static uint32_t ext80(const uint8_t *p)
{
    int e = (int)((p[0] & 0x7f) << 8 | p[1]) - 16383;
    uint32_t m = be32(p + 2);                   /* the high 32 bits are enough */
    if (p[0] & 0x80 || e < 0 || e > 31)
        return 0;
    return m >> (31 - e);
}

/* Shared by both containers once the format is read: frame size, the ring's frame */
static int set_format(uint32_t ch, uint32_t bits, uint32_t rate)
{
    if (ch < 1 || ch > 2 || (bits != 8 && bits != 16 && bits != 24 && bits != 32) || !rate)
        return -HP_ERR_UNSUPPORTED;
    ctl->rate = rate;
    ctl->channels = ch;
    ctl->bits = bits;
    ctl->frame_bytes = bits == 16 ? 4 : 8;
    block = ch * bits / 8;
    return 0;
}

/* At the first sample: the length, and the start for a seek (exact: whole frames skipped) */
static int data_start(uint32_t size, uint32_t frames)
{
    data_left = size;
    ctl->total_frames = frames < size / block ? frames : size / block;
    if (ctl->seek_ms) {
        uint64_t f = (uint64_t)ctl->seek_ms * ctl->rate / 1000;
        if (f >= ctl->total_frames)
            f = ctl->total_frames ? ctl->total_frames - 1 : 0;
        if (f && in_seek(ctl->in_rd + (uint32_t)f * block) == 0) {
            data_left -= (uint32_t)f * block;
            ctl->start_frame = (uint32_t)f;
        }
    }
    return 0;
}

int codec_open(void)
{
    uint8_t h[16];
    static int form;                            /* 1 RIFF/WAVE, 2 FORM/AIFF, 3 FORM/AIFC */
    static int fmt;
    static uint32_t skip, frames = 0xffffffffu;

    if (!form) {
        if (in_avail() < 12)
            return in_ended() ? -HP_ERR_FORMAT : 1;
        in_read(h, 12);
        if (le32(h) == 0x46464952 && le32(h + 8) == 0x45564157)          /* "RIFF", "WAVE" */
            form = 1;
        else if (le32(h) == 0x4d524f46 && le32(h + 8) == 0x46464941)     /* "FORM", "AIFF" */
            form = 2;
        else if (le32(h) == 0x4d524f46 && le32(h + 8) == 0x43464941)     /* "FORM", "AIFC" */
            form = 3;
        else
            return -HP_ERR_FORMAT;
        big = signed8 = form > 1;
    }
    for (;;) {                                  /* chunks up to the sample data */
        if (skip) {                             /* the rest of a chunk we do not use */
            uint32_t n = in_avail() < skip ? in_avail() : skip;
            in_skip(n);
            skip -= n;
            if (skip)
                return in_ended() ? -HP_ERR_FORMAT : 1;
        }
        if (in_avail() < 8)
            return in_ended() ? -HP_ERR_FORMAT : 1;
        in_peek(h, 8);
        uint32_t id = le32(h), size = form == 1 ? le32(h + 4) : be32(h + 4);
        if (form == 1 && id == 0x61746164) {    /* "data" */
            if (!fmt)
                return -HP_ERR_FORMAT;
            in_skip(8);
            return data_start(size, 0xffffffffu);
        }
        if (form > 1 && id == 0x444e5353) {    /* "SSND": offset, block size, samples */
            if (!fmt || size < 8)
                return -HP_ERR_FORMAT;
            if (in_avail() < 16)
                return in_ended() ? -HP_ERR_FORMAT : 1;
            in_skip(8);
            in_read(h, 8);
            uint32_t off = be32(h);
            if (off > size - 8 || in_seek(ctl->in_rd + off))
                return -HP_ERR_FORMAT;
            return data_start(size - 8 - off, frames);
        }
        if ((form == 1 && id == 0x20746d66) || (form > 1 && id == 0x4d4d4f43)) {   /* "fmt ", "COMM" */
            uint8_t f[40];
            uint32_t n = size < sizeof f ? size : sizeof f;
            if (size < (form == 1 ? 16u : form == 2 ? 18u : 22u))
                return -HP_ERR_FORMAT;
            if (in_avail() < 8 + n)
                return in_ended() ? -HP_ERR_FORMAT : 1;
            in_skip(8);
            in_read(f, n);
            skip = size - n + (size & 1);       /* AIFF-C: the rest of the compression name */
            int r;
            if (form == 1) {
                uint32_t tag = le16(f);
                if (tag == 0xfffe && size >= 26)
                    tag = le16(f + 24);         /* subformat GUID: its first two bytes */
                if (tag != 1)
                    return -HP_ERR_UNSUPPORTED;
                r = set_format(le16(f + 2), le16(f + 14), le32(f + 4));
            } else {
                if (form == 3) {
                    uint32_t c = le32(f + 18);
                    if (c == 0x74776f73)        /* "sowt": little-endian */
                        big = 0;
                    else if (c != 0x454e4f4e && c != 0x736f7774)    /* "NONE", "twos" */
                        return -HP_ERR_UNSUPPORTED;
                }
                frames = be32(f + 2);
                r = set_format(be16(f), (be16(f + 6) + 7) & ~7u, ext80(f + 8));
            }
            if (r)
                return r;
            fmt = 1;
            continue;
        }
        skip = 8 + size + (size & 1);           /* any other chunk (LIST, ID3, cover art) */
    }
}

static int32_t sample(const uint8_t *p, uint32_t bits)
{
    if (big)
        switch (bits) {
        case 8:  return (int32_t)((uint32_t)p[0] << 24);
        case 16: return (int32_t)(be16(p) << 16);
        case 24: return (int32_t)((uint32_t)(p[0] << 16 | p[1] << 8 | p[2]) << 8);
        default: return (int32_t)be32(p);
        }
    switch (bits) {
    case 8:  return (int32_t)((uint32_t)(signed8 ? p[0] : p[0] ^ 0x80) << 24);
    case 16: return (int32_t)((uint32_t)le16(p) << 16);
    case 24: return (int32_t)((uint32_t)(p[0] | p[1] << 8 | p[2] << 16) << 8);
    default: return (int32_t)le32(p);
    }
}

/* Whole frames in chunks of up to 4 KiB: 16-bit stereo, already the ring's packed frame, is copied
 * as it is (big-endian: the bytes of each sample swapped first); the other formats are converted
 * frame by frame from the chunk */
static uint32_t chunk[1024];

int codec_run(void)
{
    uint32_t fb = ctl->frame_bytes, bits = ctl->bits, ch = ctl->channels;

    if (data_left < block || (in_avail() < block && in_ended()))
        return 2;
    uint32_t n = in_avail() / block, room = out_space() / fb;
    if (n > room)
        n = room;
    if (n > sizeof chunk / block)
        n = sizeof chunk / block;
    if (n > data_left / block)
        n = data_left / block;
    if (!n)
        return 0;
    in_read(chunk, n * block);
    data_left -= n * block;
    if (bits == 16 && ch == 2) {
        if (big)
            for (uint32_t i = 0; i < n; i++) {
                uint32_t w = chunk[i];
                chunk[i] = (w & 0x00ff00ffu) << 8 | (w >> 8 & 0x00ff00ffu);
            }
        out_write(chunk, n);
    } else {
        const uint8_t *f = (const uint8_t *)chunk;
        for (uint32_t i = 0; i < n; i++, f += block) {
            int32_t l = sample(f, bits), r = ch == 2 ? sample(f + bits / 8, bits) : l;
            if (fb == 4)
                out_word((uint32_t)l >> 16 | ((uint32_t)r & 0xffff0000u));
            else {
                out_word((uint32_t)l);
                out_word((uint32_t)r);
            }
        }
    }
    out_done(n);
    return 1;
}
