/* WAV (RIFF, PCM or WAVE_FORMAT_EXTENSIBLE) for our core B runtime: 8/16/24/32-bit integer,
 * mono or stereo. 16-bit sources go to the ring as packed 16-bit frames (4 bytes), the
 * others as 32-bit frames (8 bytes, data in the high bits); mono is duplicated. */
#include "rt.h"

const uint32_t codec_id = HP_CODEC_WAV;

static uint32_t data_left, block;           /* bytes of sample data left, bytes per frame */

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int codec_open(void)
{
    uint8_t h[12];
    static int riff, fmt;
    static uint32_t skip;

    if (!riff) {
        if (in_avail() < 12)
            return in_ended() ? -HP_ERR_FORMAT : 1;
        in_read(h, 12);
        if (le32(h) != 0x46464952 || le32(h + 8) != 0x45564157)   /* "RIFF", "WAVE" */
            return -HP_ERR_FORMAT;
        riff = 1;
    }
    for (;;) {                                  /* chunks up to "data" */
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
        uint32_t id = le32(h), size = le32(h + 4);
        if (id == 0x61746164) {                 /* "data" */
            if (!fmt)
                return -HP_ERR_FORMAT;
            in_skip(8);
            data_left = size;
            ctl->total_frames = size / block;
            if (ctl->seek_ms) {                 /* exact: whole frames skipped */
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
        if (id == 0x20746d66) {                 /* "fmt " */
            uint8_t f[40];
            if (size < 16 || size > sizeof f)
                return -HP_ERR_FORMAT;
            if (in_avail() < 8 + size)
                return in_ended() ? -HP_ERR_FORMAT : 1;
            in_skip(8);
            in_read(f, size);
            if (size & 1)
                in_skip(1);
            uint32_t tag = le16(f), ch = le16(f + 2), rate = le32(f + 4), bits = le16(f + 14);
            if (tag == 0xfffe && size >= 26)
                tag = le16(f + 24);             /* subformat GUID: its first two bytes */
            if (tag != 1 || ch < 1 || ch > 2 || (bits != 8 && bits != 16 && bits != 24 && bits != 32))
                return -HP_ERR_UNSUPPORTED;
            ctl->rate = rate;
            ctl->channels = ch;
            ctl->bits = bits;
            ctl->frame_bytes = bits == 16 ? 4 : 8;
            block = ch * bits / 8;
            fmt = 1;
            continue;
        }
        skip = 8 + size + (size & 1);           /* any other chunk (LIST, id3, cover art) */
    }
}

static int32_t sample(const uint8_t *p, uint32_t bits)
{
    switch (bits) {
    case 8:  return (int32_t)((uint32_t)(p[0] ^ 0x80) << 24);
    case 16: return (int32_t)((uint32_t)le16(p) << 16);
    case 24: return (int32_t)((uint32_t)(p[0] | p[1] << 8 | p[2] << 16) << 8);
    default: return (int32_t)le32(p);
    }
}

/* Whole frames in chunks of up to 4 KiB: 16-bit stereo, already the ring's packed frame, is copied
 * as it is; the other formats are converted frame by frame from the chunk */
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
    if (bits == 16 && ch == 2)
        out_write(chunk, n);
    else {
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
    ctl->frames += n;
    return 1;
}
