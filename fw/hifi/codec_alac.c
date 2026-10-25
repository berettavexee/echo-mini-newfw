/* ALAC (Apple Lossless) for our core B runtime (ETUDE §56-57), with our decoder (alacdec.c,
 * derived from Apple's reference decoder, Apache-2.0). Core A reads the MP4 container
 * (src/mp4.c) and feeds the packet stream of hifi_proto.h: the header, the ALACSpecificConfig
 * (24 bytes), then each packet with its length. 16-bit sources go to the ring as packed 16-bit
 * frames (4 bytes), 20 and 24-bit ones as 32-bit frames (data in the high bits); mono is
 * duplicated. Packets are independent: a seek needs no pre-roll, only 'skip'. */
#include <string.h>
#include "rt.h"
#include "alacdec.h"

const uint32_t codec_id = HP_CODEC_ALAC;

static struct alac dec;
static uint8_t *pkt;
static uint32_t *pcm;                   /* a packet decoded, in the ring's format */
static uint32_t pkt_max, skip, left;

static void *alloc(uint32_t n)
{
    return rt_alloc(n);
}

int codec_open(void)
{
    struct hp_pkt_header h;
    uint8_t cfg[64];
    if (in_read_wait(&h, sizeof h) < sizeof h || h.magic != HP_PKT_MAGIC)
        return -HP_ERR_FORMAT;
    if (h.config_len > sizeof cfg || in_read_wait(cfg, h.config_len) < h.config_len)
        return -HP_ERR_FORMAT;
    if (alac_init(&dec, cfg, h.config_len, alloc))
        return -HP_ERR_UNSUPPORTED;
    /* the largest packet: maxFrameBytes when the encoder gave it, else uncompressed plus room for
     * the headers; 8 bytes more, read (not used) by the bit reader past the end */
    pkt_max = dec.max_frame_bytes ? dec.max_frame_bytes : dec.frame_len * dec.channels * 4 + 64;
    if (pkt_max > 64 * 1024)
        return -HP_ERR_UNSUPPORTED;
    pkt = rt_alloc(pkt_max + 8);
    pcm = rt_alloc(dec.frame_len * (dec.bits == 16 ? 4 : 8));
    if (!pkt || !pcm)
        return -HP_ERR_UNSUPPORTED;
    skip = h.skip;
    left = h.length == 0xffffffffu ? 0xffffffffu :
           h.length > h.start_frame ? h.length - h.start_frame : 0;
    ctl->rate = dec.rate ? dec.rate : h.rate;
    ctl->channels = dec.channels;
    ctl->bits = dec.bits;
    ctl->frame_bytes = dec.bits == 16 ? 4 : 8;
    ctl->total_frames = h.length == 0xffffffffu ? 0 : h.length;
    ctl->start_frame = h.start_frame;
    return 0;
}

int codec_run(void)
{
    uint8_t lb[2];
    if (!left || in_read_wait(lb, 2) < 2)
        return 2;
    uint32_t n = lb[0] | lb[1] << 8, got = in_read_wait(pkt, n < pkt_max ? n : pkt_max);
    if (n > pkt_max)
        in_skip(n - pkt_max);
    if (got < (n < pkt_max ? n : pkt_max))
        return 2;                               /* the stream ended inside a packet */
    uint32_t k = 0, wpf = dec.bits == 16 ? 1 : 2;   /* words per frame */
    memset(pkt + got, 0, 8);
    if (n > pkt_max || alac_decode(&dec, pkt, n, pcm, &k) || k > dec.frame_len) {
        k = dec.frame_len;                      /* damage: a packet of silence */
        memset(pcm, 0, k * wpf * 4);
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
    const uint32_t *p = pcm + s * wpf;
    while (k) {
        uint32_t m = out_space() / (wpf * 4);
        if (!m) {
            rt_wait();
            continue;
        }
        if (m > k)
            m = k;
        out_write(p, m * wpf);
        p += m * wpf;
        out_done(m);
        k -= m;
    }
    return 1;
}
