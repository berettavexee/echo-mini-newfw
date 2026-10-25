/* ALAC (Apple Lossless) for our core B runtime (ETUDE §56), with Apple's reference decoder
 * (third_party/alac, Apache-2.0, unmodified; C++ without exceptions or RTTI). Core A reads the
 * MP4 container (src/mp4.c) and feeds the packet stream of hifi_proto.h: the header, the
 * ALACSpecificConfig (24 bytes), then each packet with its length. 16-bit sources go to the ring
 * as packed 16-bit frames (4 bytes), 20 and 24-bit ones as 32-bit frames (data in the high
 * bits); mono is duplicated. Packets are independent: a seek needs no pre-roll, only 'skip'. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include "rt.h"
}
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"

extern "C" const uint32_t codec_id = HP_CODEC_ALAC;

/* The decoder's buffers (calloc in ALACDecoder::Init) from the runtime's heap, freed all at once
 * at the next open; operator new placed in static storage (no constructors run at start-up) */
extern "C" void *calloc(size_t a, size_t b)
{
    uint32_t n = (uint32_t)(a * b);
    void *p = rt_alloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}
extern "C" void free(void *p) { (void)p; }
void *operator new(size_t, void *p) noexcept { return p; }

static uint8_t dec_mem[sizeof(ALACDecoder)] __attribute__((aligned(8)));
static ALACDecoder *dec;
static uint8_t *pkt, *pcm;              /* a packet, its decoded samples (interleaved) */
static uint32_t pkt_max, frame_len, bytes;  /* bytes: per sample in pcm (2, 3 or 4) */
static uint32_t skip, left;

extern "C" int codec_open(void)
{
    struct hp_pkt_header h;
    uint8_t cfg[64];
    if (in_read_wait(&h, sizeof h) < sizeof h || h.magic != HP_PKT_MAGIC)
        return -HP_ERR_FORMAT;
    if (h.config_len > sizeof cfg || in_read_wait(cfg, h.config_len) < h.config_len)
        return -HP_ERR_FORMAT;
    dec = new (dec_mem) ALACDecoder();
    if (dec->Init(cfg, h.config_len) != ALAC_noErr)
        return -HP_ERR_UNSUPPORTED;
    const ALACSpecificConfig &c = dec->mConfig;
    frame_len = c.frameLength;
    if (c.numChannels < 1 || c.numChannels > 2 || !frame_len || frame_len > 16384 ||
        (c.bitDepth != 16 && c.bitDepth != 20 && c.bitDepth != 24))
        return -HP_ERR_UNSUPPORTED;
    bytes = c.bitDepth == 16 ? 2 : 3;
    /* the largest packet: maxFrameBytes when the encoder gave it, else uncompressed plus room
     * for the headers */
    pkt_max = c.maxFrameBytes ? c.maxFrameBytes : frame_len * c.numChannels * 4 + 64;
    if (pkt_max > 64 * 1024)
        return -HP_ERR_UNSUPPORTED;
    pkt = (uint8_t *)rt_alloc(pkt_max);
    pcm = (uint8_t *)rt_alloc(frame_len * c.numChannels * bytes + 16);
    if (!pkt || !pcm)
        return -HP_ERR_UNSUPPORTED;
    skip = h.skip;
    left = h.length == 0xffffffffu ? 0xffffffffu :
           h.length > h.start_frame ? h.length - h.start_frame : 0;
    ctl->rate = c.sampleRate ? c.sampleRate : h.rate;
    ctl->channels = c.numChannels;
    ctl->bits = c.bitDepth;
    ctl->frame_bytes = bytes == 2 ? 4 : 8;
    ctl->total_frames = h.length == 0xffffffffu ? 0 : h.length;
    ctl->start_frame = h.start_frame;
    return 0;
}

static int32_t s24(const uint8_t *p)            /* packed little-endian, into the high bits */
{
    return (int32_t)((uint32_t)(p[0] | p[1] << 8 | p[2] << 16) << 8);
}

extern "C" int codec_run(void)
{
    uint8_t lb[2];
    if (!left || in_read_wait(lb, 2) < 2)
        return 2;
    uint32_t n = lb[0] | lb[1] << 8, got = in_read_wait(pkt, n < pkt_max ? n : pkt_max);
    if (n > pkt_max)
        in_skip(n - pkt_max);
    if (got < (n < pkt_max ? n : pkt_max))
        return 2;                               /* the stream ended inside a packet */
    uint32_t ch = dec->mConfig.numChannels, k = 0;
    int err = n > pkt_max;
    if (!err) {
        BitBuffer bb;
        BitBufferInit(&bb, pkt, n);
        err = dec->Decode(&bb, pcm, frame_len, ch, &k) != ALAC_noErr || k > frame_len;
    }
    if (err) {                                  /* damage: a packet of silence */
        k = frame_len;
        memset(pcm, 0, frame_len * ch * bytes);
        ctl->error_info++;
    }
    uint32_t s = skip < k ? skip : k;
    skip -= s;
    k -= s;
    if (k > left)
        k = left;
    if (left != 0xffffffffu)
        left -= k;
    /* in pieces as room comes: 4096 frames of 24-bit fill the whole output ring (32 KiB) */
    uint32_t fb = ctl->frame_bytes;
    const uint8_t *p = pcm + s * ch * bytes;
    while (k) {
        uint32_t m = out_space() / fb;
        if (!m) {
            rt_wait();
            continue;
        }
        if (m > k)
            m = k;
        if (bytes == 2 && ch == 2) {
            out_write((const uint32_t *)p, m);  /* already the packed frame */
            p += m * 4;
        } else
            for (uint32_t i = 0; i < m; i++, p += ch * bytes) {
                if (bytes == 2) {
                    uint32_t l = (uint16_t)(p[0] | p[1] << 8);
                    out_word(l | l << 16);      /* mono */
                } else {
                    int32_t l = s24(p);
                    out_word((uint32_t)l);
                    out_word((uint32_t)(ch == 2 ? s24(p + 3) : l));
                }
            }
        ctl->frames += m;
        k -= m;
    }
    return 1;
}
