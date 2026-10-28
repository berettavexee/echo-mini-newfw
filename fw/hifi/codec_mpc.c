/* Musepack (SV7 and SV8) for our core B runtime (ETUDE §61), with libmpcdec
 * (third_party/libmpcdec, BSD 3-clause) in fixed point; its synthesis on SYNTH (mpcsynth.c). Its reader reads the input
 * ring; a seek backwards (SV8: the seek table at the end of the file) asks core A to feed again
 * from there (in_seek). libmpcdec removes the decoder delay and gives the exact length; a seek
 * goes through mpc_demux_seek_sample from a fresh open (SV7: it walks the frame headers from
 * the start). 16-bit packed stereo frames in the ring (the reference tool's >> FRACTPART), mono
 * duplicated. */
#include <math.h>
#include <string.h>
#include "hooks.h"
#include "rt.h"
#include <mpc/mpcdec.h>
#ifdef ECHO_SYNTH
#include "mpcsynth.c"
#endif

const uint32_t codec_id = HP_CODEC_MPC;

/* ---- what libmpcdec takes from the C library ---- */
void *malloc(size_t n)
{
    return rt_alloc(n);
}

void *calloc(size_t a, size_t b)
{
    void *p = rt_alloc(a * b);
    if (p)
        memset(p, 0, a * b);
    return p;
}

void free(void *p)
{
    (void)p;                                    /* the heap is reset at the next open */
}

/* Outside the decoding: the encoder string (sprintf), ReplayGain in dB from the header (pow,
 * log10: core A reads ReplayGain from the APEv2 tag itself), the CRC of SV8 blocks */
int sprintf(char *s, const char *f, ...)
{
    (void)f;
    s[0] = 0;
    return 0;
}

double pow(double a, double b)
{
    (void)a;
    (void)b;
    return 1.0;
}

double log10(double a)
{
    (void)a;
    return 0.0;
}

unsigned long mpc_crc32(unsigned char *buf, int len)
{
    uint32_t c = 0xffffffffu;
    while (len-- > 0) {
        c ^= *buf++;
        for (int k = 0; k < 8; k++)
            c = c >> 1 ^ (0xedb88320u & -(c & 1));
    }
    return c ^ 0xffffffffu;
}

/* ---- the reader over the input ring ---- */
static mpc_int32_t r_read(mpc_reader *r, void *p, mpc_int32_t n)
{
    (void)r;
    return n > 0 ? (mpc_int32_t)in_read_wait(p, (uint32_t)n) : 0;
}

static mpc_bool_t r_seek(mpc_reader *r, mpc_int32_t off)
{
    (void)r;
    return off >= 0 && in_seek((uint32_t)off) == 0;
}

static mpc_int32_t r_tell(mpc_reader *r)
{
    (void)r;
    return (mpc_int32_t)ctl->in_rd;
}

static mpc_int32_t r_size(mpc_reader *r)
{
    (void)r;
    return ctl->in_end == 0xffffffffu ? 0x7fffffff : (mpc_int32_t)ctl->in_end;
}

static mpc_bool_t r_canseek(mpc_reader *r)
{
    (void)r;
    return MPC_TRUE;
}

static mpc_reader reader = { r_read, r_seek, r_tell, r_size, r_canseek, 0 };
static mpc_demux *demux;
static MPC_SAMPLE_FORMAT buf[MPC_DECODER_BUFFER_LENGTH];
static uint32_t ch;

int codec_open(void)
{
    mpc_streaminfo si;
#ifdef ECHO_SYNTH
    echo_mpc_synth_reset();
#endif
    demux = mpc_demux_init(&reader);
    if (!demux)
        return -HP_ERR_FORMAT;
    mpc_demux_get_info(demux, &si);
    if (si.channels < 1 || si.channels > 2 || !si.sample_freq)
        return -HP_ERR_UNSUPPORTED;
    ch = si.channels;
    uint64_t total = si.samples > si.beg_silence ? si.samples - si.beg_silence : 0;
    ctl->rate = si.sample_freq;
    ctl->channels = ch;
    ctl->bits = 16;
    ctl->frame_bytes = 4;
    ctl->total_frames = total > 0xffffffffu ? 0 : (uint32_t)total;
    if (ctl->seek_ms && total) {
        uint64_t want = (uint64_t)ctl->seek_ms * si.sample_freq / 1000;
        if (want < total && mpc_demux_seek_sample(demux, want) == MPC_STATUS_OK)
            ctl->start_frame = (uint32_t)want;
    }
    return 0;
}

static inline uint32_t s16(MPC_SAMPLE_FORMAT v)
{
    int32_t x = v >> MPC_FIXED_POINT_FRACTPART;
    return (uint16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

int codec_run(void)
{
    mpc_frame_info fi;
    fi.buffer = buf;
    if (mpc_demux_decode(demux, &fi) != MPC_STATUS_OK) {
        ctl->error_info++;
        return 2;                               /* damaged beyond what the demuxer skips */
    }
    if (fi.bits == -1)
        return 2;                               /* the end of the stream */
    uint32_t n = fi.samples;
    while (out_space() < n * 4)
        rt_wait();
    for (uint32_t i = 0; i < n; i++)
        out_word(s16(buf[i * ch]) | s16(buf[i * ch + ch - 1]) << 16);
    out_done(n);
    return 1;
}
