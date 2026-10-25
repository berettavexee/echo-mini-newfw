/* Ogg Opus (RFC 7845) for our core B runtime, with libopus 1.5.2 in fixed point (the variant
 * validated on the device: ARM multiply macros, encoder-only functions stubbed). Always 48 kHz
 * stereo 16-bit (mono streams are duplicated by the decoder); pre-skip removed at the start,
 * the end trimmed to the granule of the last page. */
#include <string.h>
#include "rt.h"
#include "oggpage.h"
#include "opus.h"

#ifndef GLOBAL_STACK_SIZE
#error GLOBAL_STACK_SIZE
#endif
#define MAX_FRAME   5760                /* 120 ms at 48 kHz: the longest packet */
#define MAX_PACKET  8192

const uint32_t codec_id = HP_CODEC_OPUS;

char opus_scratch[GLOBAL_STACK_SIZE] __attribute__((aligned(8)));
static uint8_t dec_mem[DEC_MEM_SIZE] __attribute__((aligned(8)));
static uint8_t pkt[MAX_PACKET] __attribute__((aligned(8)));
static int16_t pcm[MAX_FRAME * 2] __attribute__((aligned(8)));

static OpusDecoder *dec;
static int64_t pos;                     /* 48 kHz position of the next decoded sample */
static int64_t discard;                 /* output dropped before this position (pre-skip, seek) */
static uint32_t pre_skip;
static int end, resync;                 /* resync: after a seek, position unknown until a granule */

int codec_open(void)
{
    pg_reset();
    pos = 0;
    end = 0;
    int n = pg_packet(pkt, sizeof pkt);    /* identification header */
    if (n < 19 || memcmp(pkt, "OpusHead", 8) || pkt[8] >> 4)
        return -HP_ERR_FORMAT;
    uint32_t ch = pkt[9], family = pkt[18];
    if (ch < 1 || ch > 2 || family > 1)
        return -HP_ERR_UNSUPPORTED;
    pre_skip = pkt[10] | pkt[11] << 8;
    int16_t gain = (int16_t)(pkt[16] | pkt[17] << 8);
    if ((uint32_t)opus_decoder_get_size(2) > sizeof dec_mem)
        return -HP_ERR_UNSUPPORTED;
    dec = (OpusDecoder *)dec_mem;
    if (opus_decoder_init(dec, 48000, 2) != OPUS_OK)
        return -HP_ERR_DECODE;
    if (gain)
        opus_decoder_ctl(dec, OPUS_SET_GAIN(gain));
    /* comment header (tags, cover art): only its magic is read, the rest is skipped */
    if (ctl->comment_end_page) {           /* core A found where it ends (tags.c) */
        if (pg_skip_to(ctl->comment_end_page, ctl->comment_end_seg))
            return -HP_ERR_FORMAT;
    } else {
        n = pg_packet(pkt, 8);
        if (n == 0 || (n > 0 && n < 8) || memcmp(pkt, "OpusTags", 8))
            return -HP_ERR_FORMAT;
    }
    uint64_t g = ctl->end_granule_lo | (uint64_t)ctl->end_granule_hi << 32;
    ctl->rate = 48000;
    ctl->channels = ch;
    ctl->bits = 16;
    ctl->frame_bytes = 4;
    ctl->total_frames = g > pre_skip ? (uint32_t)(g - pre_skip) : 0;
    discard = pre_skip;
    if (ctl->seek_ms && ctl->total_frames && ctl->in_end != 0xffffffffu) {
        /* a byte in proportion to the stream a second before the target, then the next page;
         * decoded and dropped until a page granule gives the position, then up to the target */
        uint64_t want = (uint64_t)ctl->seek_ms * 48, total = ctl->total_frames;
        uint64_t aim = want > 48000 ? want - 48000 : 0;
        uint32_t data = ctl->in_rd;
        uint32_t off = ctl->seek_offset ? ctl->seek_offset :   /* core A found the page */
                       data + (uint32_t)((uint64_t)(ctl->in_end - data) * aim / total);
        if (want < total && ctl->in_end > data && !pg_jump(off)) {
            opus_decoder_ctl(dec, OPUS_RESET_STATE);
            discard = want + pre_skip;
            resync = 1;
        }
    }
    return 0;
}

int codec_run(void)
{
    if (end)
        return 2;
    int len = pg_packet(pkt, sizeof pkt);
    if (len == 0)
        return 2;
    if (len < 0)
        return 1;                               /* too long for an Opus packet: dropped */
    int n = opus_decode(dec, pkt, len, pcm, MAX_FRAME, 0);
    if (n < 0) {
        ctl->error_info++;                      /* damaged packet: dropped */
        return 1;
    }
    if (resync) {                               /* dropped; the granule ends this packet */
        if (pg_last_on_page() && pg.granule >= 0) {
            pos = pg.granule;
            resync = 0;
            ctl->start_frame = (uint32_t)((pos > discard ? pos : discard) - pre_skip);
        }
        return 1;
    }
    int skip = 0;
    if (pos < discard)
        skip = discard - pos < n ? (int)(discard - pos) : n;
    if ((pg.flags & 4) && pg_last_on_page() && pg.granule >= 0 && pos + n > pg.granule) {
        int64_t keep = pg.granule - pos;       /* last page: end trimming */
        n = keep < 0 ? 0 : (int)keep;
        if (skip > n)
            skip = n;
        end = 1;
    }
    pos += n;
    uint32_t frames = (uint32_t)(n - skip);
    const uint32_t *w = (const uint32_t *)(pcm + skip * 2);
    while (frames) {                            /* a packet (up to 23 KiB) in pieces, as the */
        uint32_t k = out_space() / 4;           /* output ring frees up */
        if (!k) {
            rt_wait();
            continue;
        }
        if (k > frames)
            k = frames;
        for (uint32_t i = 0; i < k; i++)
            out_word(w[i]);
        w += k;
        frames -= k;
        out_done(k);
    }
    return 1;
}
