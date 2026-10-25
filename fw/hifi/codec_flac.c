/* FLAC for our core B runtime, with dr_flac (hifi/ext, public domain / MIT-0). dr_flac pulls
 * its input through the read callback, which waits in rt_wait() for core A to fill the input
 * ring; metadata blocks (cover art) are skipped with in_seek(), so core A jumps over them.
 * 16-bit sources go to the ring as packed 16-bit frames, the others as 32-bit frames; mono is
 * duplicated, more than two channels are refused. The stream ends with the frame count of
 * STREAMINFO (zeros or garbage after it are not read); a frame that does not decode (damaged in
 * the middle) is passed over: the decoder looks for the next frame sync code. */
#include <stddef.h>
#include <string.h>
#include "rt.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_WCHAR
#define NDEBUG
#define DRFLAC_ASSERT(x) ((void)0)
#define DRFLAC_MALLOC(sz) rt_alloc(sz)
#define DRFLAC_REALLOC(p, sz) flac_realloc(p, sz)
#define DRFLAC_FREE(p) ((void)(p))
static void *flac_realloc(void *p, size_t n);
#include "ext/dr_flac.h"

const uint32_t codec_id = HP_CODEC_FLAC;

static drflac *flac;
static union { drflac_int16 s16[1024 * 2]; drflac_int32 s32[512 * 2]; } pcm;

/* dr_flac reallocates only for metadata it keeps; a fresh block is enough with our heap */
static void *flac_realloc(void *p, size_t n)
{
    void *q = rt_alloc(n);
    if (q && p)
        memcpy(q, p, n);                        /* the old block is at least as long: heap */
    return q;
}

static size_t on_read(void *u, void *out, size_t n)
{
    (void)u;
    return in_read_wait(out, n);
}

static drflac_bool32 on_seek(void *u, int off, drflac_seek_origin o)
{
    (void)u;
    if (o != DRFLAC_SEEK_CUR && o != DRFLAC_SEEK_SET)
        return DRFLAC_FALSE;
    uint32_t pos = o == DRFLAC_SEEK_CUR ? ctl->in_rd + (uint32_t)off : (uint32_t)off;
    return in_seek(pos) == 0;
}

static drflac_bool32 on_tell(void *u, drflac_int64 *c)
{
    (void)u;
    *c = ctl->in_rd;
    return DRFLAC_TRUE;
}

/* Start at ctl->seek_ms: a byte in proportion to the stream a second before the target, at or
 * after the input already read (only forward), the FLAC frame found there decoded, then forward
 * to the target; ctl->start_frame = the position reached. Left at the start when it fails. */
static void seek(void)
{
    uint64_t total = flac->totalPCMFrameCount, want = (uint64_t)ctl->seek_ms * flac->sampleRate / 1000;
    uint64_t first = flac->firstFLACFramePosInBytes, len = ctl->in_end - first, last;
    if (!total || want >= total || ctl->in_end == 0xffffffffu || ctl->in_end <= first)
        return;
    uint64_t aim = want > flac->sampleRate ? want - flac->sampleRate : 0;
    uint64_t byte = first + len * aim / total;
    if (byte <= ctl->in_rd)
        byte = ctl->in_rd + 1;
    if (byte >= ctl->in_end || !drflac__seek_to_approximate_flac_frame_to_byte(flac, byte, byte,
                                                                                ctl->in_end, &last))
        return;
    if (flac->currentPCMFrame < want)
        drflac__seek_forward_by_pcm_frames(flac, want - flac->currentPCMFrame);
    ctl->start_frame = (uint32_t)flac->currentPCMFrame;
}

int codec_open(void)
{
    rt_heap_reset();
    flac = drflac_open(on_read, on_seek, on_tell, 0, 0);
    if (!flac)
        return -HP_ERR_FORMAT;
    if (flac->channels < 1 || flac->channels > 2)
        return -HP_ERR_UNSUPPORTED;
    ctl->rate = flac->sampleRate;
    ctl->channels = flac->channels;
    ctl->bits = flac->bitsPerSample;
    ctl->frame_bytes = flac->bitsPerSample == 16 ? 4 : 8;
    ctl->total_frames = (uint32_t)flac->totalPCMFrameCount;
    ctl->error_info = rt_heap_peak;
    if (ctl->seek_ms)
        seek();
    return 0;
}

/* Waits for room in the output ring */
static void out_wait(uint32_t bytes)
{
    while (out_space() < bytes)
        rt_wait();
}

/* Nothing decoded: the end of the stream, or a damaged frame to pass over */
static int no_frames(void)
{
    if (in_ended() && !in_avail())
        return 2;
    return 1;                                   /* the next call looks for the next sync code */
}

int codec_run(void)
{
    uint32_t ch = flac->channels, want = ctl->frame_bytes == 4 ? 1024 : 512;
    uint64_t total = flac->totalPCMFrameCount, cur = flac->currentPCMFrame;
    if (total && cur >= total)
        return 2;                               /* all the frames of STREAMINFO */
    if (total && total - cur < want)
        want = (uint32_t)(total - cur);         /* not one more: dr_flac would look for the */
    if (ctl->frame_bytes == 4) {                /* next frame through what follows */
        uint32_t n = (uint32_t)drflac_read_pcm_frames_s16(flac, want, pcm.s16);
        if (!n)
            return no_frames();
        out_wait(n * 4);
        for (uint32_t i = 0; i < n; i++) {
            uint16_t l = (uint16_t)pcm.s16[i * ch], r = (uint16_t)pcm.s16[i * ch + ch - 1];
            out_word(l | (uint32_t)r << 16);
        }
        out_done(n);
    } else {
        uint32_t n = (uint32_t)drflac_read_pcm_frames_s32(flac, want, pcm.s32);
        if (!n)
            return no_frames();
        out_wait(n * 8);
        for (uint32_t i = 0; i < n; i++) {
            out_word((uint32_t)pcm.s32[i * ch]);
            out_word((uint32_t)pcm.s32[i * ch + ch - 1]);
        }
        out_done(n);
    }
    return 1;
}
