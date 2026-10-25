/* AAC-LC for our core B runtime (ETUDE §55), with the PacketVideo decoder of AOSP
 * (third_party/pvaacdec, Apache-2.0, built without AAC+). Core A reads the MP4 container and its
 * tables and feeds a packet stream (hifi_proto.h): a header, the AudioSpecificConfig, then each
 * AAC frame with its length. The decoder gives 1024 frames of 16-bit interleaved stereo per AAC
 * frame (it duplicates mono itself: desiredChannels 2); the frames the header says to skip (the
 * encoder delay, or what comes before a seek target) and those past the length (the padding)
 * are dropped. */
#include <stdbool.h>
#include <string.h>
#include "rt.h"
#include "pvmp4audiodecoder_api.h"

#define MEM_SIZE    (52 * 1024)         /* PVMP4AudioDecoderGetMemRequirements(): 48.7 KiB */
#define FRAME_MAX   4096                /* an AAC frame: at most 6144 bits per channel */

const uint32_t codec_id = HP_CODEC_AAC;

static uint8_t mem[MEM_SIZE] __attribute__((aligned(8)));
static int16_t pcm[2048 * 2] __attribute__((aligned(4)));   /* AAC+ would use the second half */
static uint8_t frame[FRAME_MAX];
static tPVMP4AudioDecoderExternal ext;
static uint32_t skip, left;             /* frames still to drop, to write (0xffffffff: all) */

int codec_open(void)
{
    struct hp_pkt_header h;
    uint8_t cfg[64];
    if (in_read_wait(&h, sizeof h) < sizeof h || h.magic != HP_PKT_MAGIC)
        return -HP_ERR_FORMAT;
    if (h.config_len > sizeof cfg || in_read_wait(cfg, h.config_len) < h.config_len)
        return -HP_ERR_FORMAT;
    if (PVMP4AudioDecoderGetMemRequirements() > sizeof mem || !h.rate || h.channels < 1 ||
        h.channels > 2)
        return -HP_ERR_UNSUPPORTED;
    memset(&ext, 0, sizeof ext);
    ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
    ext.desiredChannels = 2;
    ext.aacPlusEnabled = false;
    if (PVMP4AudioDecoderInitLibrary(&ext, mem))
        return -HP_ERR_UNSUPPORTED;
    ext.pInputBuffer = cfg;
    ext.inputBufferCurrentLength = (Int)h.config_len;
    ext.inputBufferMaxLength = 0;
    if (PVMP4AudioDecoderConfig(&ext, mem))
        return -HP_ERR_UNSUPPORTED;            /* not AAC-LC (or a configuration it refuses) */
    skip = h.skip;
    left = h.length == 0xffffffffu ? 0xffffffffu :
           h.length > h.start_frame ? h.length - h.start_frame : 0;
    ctl->rate = h.rate;
    ctl->channels = h.channels;
    ctl->bits = 16;
    ctl->frame_bytes = 4;
    ctl->total_frames = h.length == 0xffffffffu ? 0 : h.length;
    ctl->start_frame = h.start_frame;
    return 0;
}

int codec_run(void)
{
    uint8_t lb[2];
    if (!left || in_read_wait(lb, 2) < 2)
        return 2;
    uint32_t n = lb[0] | lb[1] << 8, got = in_read_wait(frame, n < FRAME_MAX ? n : FRAME_MAX);
    if (n > FRAME_MAX)
        in_skip(n - FRAME_MAX);                 /* not a frame: decoded as silence below */
    int err = n > FRAME_MAX || got < n;
    if (!err) {
        ext.pInputBuffer = frame;
        ext.inputBufferCurrentLength = (Int)n;
        ext.inputBufferMaxLength = 0;
        ext.inputBufferUsedLength = 0;
        ext.remainderBits = 0;
        ext.pOutputBuffer = pcm;
        ext.pOutputBuffer_plus = &pcm[2048];
        ext.repositionFlag = false;
        err = PVMP4AudioDecodeFrame(&ext, mem) != MP4AUDEC_SUCCESS;
    }
    if (err) {                                  /* damage: a frame of silence, the timing kept */
        memset(pcm, 0, 1024 * 4);
        ctl->error_info++;
    }
    if (got < n)
        return 2;                               /* the stream ended inside a frame */
    uint32_t k = 1024, s = skip < k ? skip : k;
    skip -= s;
    k -= s;
    if (k > left)
        k = left;
    if (left != 0xffffffffu)
        left -= k;
    while (out_space() < k * 4)
        rt_wait();
    out_write((const uint32_t *)pcm + s, k);
    ctl->frames += k;
    return 1;
}
