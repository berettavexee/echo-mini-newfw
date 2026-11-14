/* WavPack for our core B runtime (ETUDE §156), with libwavpack 5.9.0 (third_party/wavpack,
 * BSD 3-clause) built for decoding only: no DSD, no legacy (pre-4.0) files, no tags (core A
 * reads the APEv2 tag itself), no correction file (a hybrid file plays its lossy part). Its
 * reader reads the input ring; a backward jump (the seek's search, the end of the file when the
 * length is not in the first block) asks core A to feed again from there (in_seek). libwavpack
 * reads each block whole into a buffer: its malloc is heap.c's (a block is freed when the next
 * one is read), a failure a fault (below); a seek skips to its sample in pieces (ECHO MINI). Up to 16 bits: packed 16-bit frames; more, and floats: 32-bit frames (the
 * samples left-justified, floats converted with clipping); mono duplicated, more than two
 * channels refused. */
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "hooks.h"
#include "wavpack.h"
#include "wavpack_local.h"                     /* (the first block's decorrelation terms) */

const uint32_t codec_id = HP_CODEC_WAVPACK;

void heap_reset(void);
extern uint32_t heap_peak;
void *heap_malloc(size_t n);
void *heap_calloc(size_t a, size_t b);
void *heap_realloc(void *p, size_t n);
void heap_free(void *p);

/* ---- malloc for libwavpack: heap.c's, but a failure stops core B (a fault: core A loads the
 * codec again, then passes the track over) instead of returning 0. libwavpack does not check
 * every allocation, and a write through 0 lands in our code (HIGHRAM0 is core B's address 0):
 * the next tracks then ran broken code until another codec was loaded (0.105wv1, §156). ---- */
static void *alloc_ok(void *p, size_t n)
{
    if (!p && n) {
        ctl->why = HP_WHY_MEMORY;
        __builtin_trap();
    }
    return p;
}

void *malloc(size_t n)
{
    return alloc_ok(heap_malloc(n), n);
}

void *calloc(size_t a, size_t b)
{
    return alloc_ok(heap_calloc(a, b), a * b);
}

void *realloc(void *p, size_t n)
{
    return alloc_ok(heap_realloc(p, n), n);
}

void free(void *p)
{
    heap_free(p);
}

/* ---- what libwavpack takes from the C library (no libc on core B) ---- */
char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++)
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    return 0;
}

long labs(long v);
long labs(long v)
{
    return v < 0 ? -v : v;
}

/* newlib's character classes (isalnum: an alternate file extension's check, in the metadata):
 * none here, so such an extension is ignored (it names the source file's type, not played) */
const char _ctype_[257];

/* ---- the reader over the input ring (one byte pushed back, kept here: going back one byte
 * through in_seek would make core A feed again) ---- */
static int pushed = -1;

static int32_t r_read(void *id, void *data, int32_t n)
{
    (void)id;
    uint8_t *p = data;
    int32_t got = 0;
    if (n <= 0)
        return 0;
    if (pushed >= 0) {
        *p++ = (uint8_t)pushed;
        pushed = -1;
        got = 1;
        n--;
    }
    return got + (n ? (int32_t)in_read_wait(p, (uint32_t)n) : 0);
}

static int64_t r_pos(void *id)
{
    (void)id;
    return (int64_t)ctl->in_rd - (pushed >= 0);
}

static int64_t r_len(void *id)
{
    (void)id;
    return ctl->in_end == 0xffffffffu ? 0 : (int64_t)ctl->in_end;
}

static int r_abs(void *id, int64_t pos)
{
    (void)id;
    pushed = -1;
    return pos < 0 || pos > 0xffffffffu || in_seek((uint32_t)pos) ? -1 : 0;
}

static int r_rel(void *id, int64_t d, int mode)
{
    int64_t base = mode == 1 ? r_pos(id) : mode == 2 ? r_len(id) : 0;   /* SEEK_CUR, SEEK_END */
    return r_abs(id, base + d);
}

static int r_push(void *id, int c)
{
    (void)id;
    pushed = c & 0xff;
    return c;
}

static int r_can(void *id)
{
    (void)id;
    return 1;
}

static WavpackStreamReader64 reader = { r_read, 0, r_pos, r_abs, r_rel, r_push, r_len, r_can, 0,
                                        0 };
static WavpackContext *wpc;
static uint32_t ch, shift, flt, int16;
#define CHUNK 512                               /* frames a call */
static int32_t buf[CHUNK * 2];

/* ---- too heavy for core B (§157): its cycles a second estimated from the first block, as
 * measured on the device (0.105wv3, the Thumb-2 passes, whole tracks of 30 s): a sample (rate x
 * channels) costs C0 + C1 cycles per decorrelation term; 19/16 of that past 16 bits, 23/16 for
 * 32-bit integers and floats, 4/3 for hybrid (lossy) files. Over WV_MAX_MHZ the file is refused,
 * with its reason, rather than played with gaps: core B's top step is 200 MHz, and the 24/192
 * normal mode measured 174 MHz played without a gap, high (about 230) did not. ---- */
#define WV_C0     258
#define WV_C1     25
#ifndef WV_MAX_MHZ
#define WV_MAX_MHZ 190
#endif

static uint32_t wv_mhz(uint32_t rate, uint32_t nch, uint32_t terms, int bits, int mode)
{
    uint64_t c = (uint64_t)rate * nch * (WV_C0 + WV_C1 * terms);
    if (bits > 24 || (mode & MODE_FLOAT))
        c = c * 23 / 16;
    else if (bits > 16)
        c = c * 19 / 16;
    if (mode & MODE_HYBRID)
        c = c * 4 / 3;
    return (uint32_t)(c / 1000000);
}

int codec_open(void)
{
    char err[80];
    heap_reset();
    pushed = -1;
    wpc = WavpackOpenFileInputEx64(&reader, 0, 0, err, 0, 0);
    if (!wpc) {
        ctl->error_info = heap_peak;
        if (!strncmp(err, "not configured to handle DSD", 28))
            return REFUSE(HP_ERR_UNSUPPORTED, HP_WHY_DSD);
        return !strncmp(err, "can't allocate", 14) ? REFUSE(HP_ERR_UNSUPPORTED, HP_WHY_MEMORY) :
               -HP_ERR_FORMAT;
    }
    int mode = WavpackGetMode(wpc), bits = WavpackGetBitsPerSample(wpc);
    if (WavpackGetQualifyMode(wpc) & QMODE_DSD_AUDIO)
        return REFUSE(HP_ERR_UNSUPPORTED, HP_WHY_DSD);
    ch = (uint32_t)WavpackGetNumChannels(wpc);
    if (ch < 1 || ch > 2)
        return REFUSE(HP_ERR_UNSUPPORTED, HP_WHY_CHANNELS);
    if (bits < 1 || bits > 32 || !WavpackGetSampleRate(wpc))
        return REFUSE(HP_ERR_UNSUPPORTED, HP_WHY_BITS);
    uint32_t terms = wpc->num_streams ? (uint32_t)wpc->streams[0]->num_terms : 0;
    uint32_t mhz = wv_mhz(WavpackGetSampleRate(wpc), ch, terms, bits, mode);
    ctl->error_info = terms << 16 | (mhz > 0xffff ? 0xffff : mhz);    /* (core A logs them) */
    if (mhz > WV_MAX_MHZ)
        return REFUSE(HP_ERR_UNSUPPORTED, HP_WHY_HEAVY);
    flt = (mode & MODE_FLOAT) != 0;
    int16 = !flt && bits <= 16;
    shift = flt ? 0 : (uint32_t)((int16 ? 16 : 32) - WavpackGetBytesPerSample(wpc) * 8);
    ctl->rate = WavpackGetSampleRate(wpc);
    ctl->channels = ch;
    ctl->bits = (uint32_t)bits;
    ctl->frame_bytes = int16 ? 4 : 8;
    int64_t total = WavpackGetNumSamples64(wpc);
    ctl->total_frames = total > 0 && total <= 0xffffffff ? (uint32_t)total : 0;
    if (ctl->seek_ms && total > 0) {
        int64_t want = (int64_t)ctl->seek_ms * ctl->rate / 1000;
        if (want < total && WavpackSeekSample64(wpc, want))
            ctl->start_frame = (uint32_t)want;
    }
    return 0;
}

/* An IEEE float (+-1.0 full scale) to a 32-bit sample, rounded to the nearest and clipped (as
 * FFmpeg converts); no FPU on core B */
static uint32_t f2s(uint32_t b)
{
    int e = (int)(b >> 23 & 0xff) - 119;        /* the mantissa's shift to 2^31 full scale */
    uint32_t m = (b & 0x7fffff) | 0x800000, v;
    if (e >= 8)
        v = 0x7fffffff;                         /* 1.0 and over (and Inf, NaN) */
    else if (e >= 0)
        v = m << e;
    else
        v = e > -25 ? (m + (1u << (-e - 1))) >> -e : 0;     /* rounded */
    if (v > 0x7fffffff)
        v = 0x7fffffff;
    return b >> 31 ? (uint32_t)-(int32_t)v : v;
}

int codec_run(void)
{
    uint32_t n = WavpackUnpackSamples(wpc, buf, CHUNK);
    if (!n)
        return 2;                               /* the end (or a block that does not read) */
    ctl->error_info = (uint32_t)WavpackGetNumErrors(wpc) << 24 | heap_peak;
    uint32_t last = ch - 1;
    if (int16) {
        while (out_space() < n * 4)
            rt_wait();
        for (uint32_t i = 0; i < n; i++)
            out_word((uint16_t)(buf[i * ch] << shift) |
                     (uint32_t)(uint16_t)(buf[i * ch + last] << shift) << 16);
    } else {
        while (out_space() < n * 8)
            rt_wait();
        for (uint32_t i = 0; i < n; i++) {
            uint32_t l = (uint32_t)buf[i * ch], r = (uint32_t)buf[i * ch + last];
            if (flt) {
                l = f2s(l);
                r = f2s(r);
            } else {
                l <<= shift;
                r <<= shift;
            }
            out_word(l);
            out_word(r);
        }
    }
    out_done(n);
    return 1;
}
