/* Ogg Vorbis for our core B runtime, with Tremor "lowmem" in full precision (re/vorbis: Xiph
 * Tremor 89a7534 + our patches, ARM multiply macros and ssat, as validated on the device in
 * HIFIEC56). Tremor's own Ogg layer is not used: oggpage.c reads the pages. The comment header
 * (tags, cover art: often 70+ KB) is skipped, Tremor gets an empty one. 16-bit output, mono
 * duplicated; the end is trimmed by Tremor from the granule of the last page, the beginning
 * here (start_trim()). The stream ends with its last page (flag 4): zeros or garbage after it
 * are not read.
 * Codebook cache: decoding the setup header takes about 2 M instructions (re/REPORT.md §38). The
 * tracks of an album share their identification and setup headers, so the decoded vorbis_info
 * and the heap blocks below it are kept in .noinit (not cleared when core B restarts), with a
 * copy of both headers and a checksum; an OPEN whose headers are the same bytes, with the
 * checksum intact, starts from them. Anything that overwrote or changed that memory (another
 * codec loaded, Tremor writing into its setup) fails the checksum: the headers are decoded. */
#include <string.h>
#include "rt.h"
#include "hooks.h"
#include "oggpage.h"
#include "ivorbiscodec.h"
#include "codec_internal.h"
#include "hififft.c"

#define MAX_PACKET   16384              /* setup header ~4 KB, audio packets < 2 KB */
#define BLOCK_FRAMES 512
#define MAX_ERRORS   64                 /* consecutive undecodable packets before the end */

const uint32_t codec_id = HP_CODEC_VORBIS;


#define CACHE_MAGIC 0x56424331u         /* "1CBV" */
#define IDENT_MAX   64
#define SETUP_MAX   8192
static struct {
    uint32_t magic, sum;                /* sum: of everything below, and of the kept heap */
    uint32_t floor, cur, ident_len, setup_len;
    vorbis_info vi;
    uint8_t ident[IDENT_MAX], setup[SETUP_MAX];
} cache __attribute__((section(".noinit"), aligned(8)));
uint32_t vorbis_cache_hits;             /* this OPEN: 1 = codebooks taken from the cache */

static uint32_t cache_sum(void)
{
    uint32_t h = 2166136261u;
    const uint32_t *w = (const uint32_t *)&cache.floor;
    const uint32_t *e = (const uint32_t *)(cache.setup + ((cache.setup_len + 3) & ~3u));
    while (w < e)
        h = (h ^ *w++) * 16777619u;
    w = (const uint32_t *)heap_base();
    e = w + cache.floor / 4;
    while (w < e)
        h = (h ^ *w++) * 16777619u;
    return h;
}

static uint8_t pkt[MAX_PACKET] __attribute__((aligned(8)));
static int16_t pcm[BLOCK_FRAMES * 2];
static vorbis_info vi;
static vorbis_comment vc;
static vorbis_dsp_state *vd;
static int64_t packetno;
static int errors, eos, last;           /* last: the packet of the last page went in */
static int resync;                      /* after a seek: position unknown until a page granule */
static int64_t vpos;                    /* position of the next frame out */
static int64_t gshift;                  /* added to the granules given to Tremor */

/* Empty comment header given to Tremor in place of the file's own */
static uint8_t empty_comment[16] = { 3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1 };

static void packet(ogg_buffer *ob, ogg_reference *ref, ogg_packet *op, uint8_t *data, int len)
{
    memset(ob, 0, sizeof *ob);
    memset(ref, 0, sizeof *ref);
    memset(op, 0, sizeof *op);
    ob->data = data;
    ob->size = len;
    ob->refcount = 1;
    ref->buffer = ob;
    ref->length = len;
    op->packet = ref;
    op->bytes = len;
    op->granulepos = -1;
    op->packetno = packetno++;
}

static int header_in(uint8_t *data, int len, int b_o_s)
{
    ogg_buffer ob;
    ogg_reference ref;
    ogg_packet op;
    packet(&ob, &ref, &op, data, len);
    op.b_o_s = b_o_s;
    return vorbis_dsp_headerin(&vi, &vc, &op);
}

/* Frames to drop at the start of the stream so that the first frame out is at granule 0: a
 * first audio page whose granule is below the frames its packets give trims the beginning
 * (Vorbis I §A.2; ffmpeg's encoder makes such streams). Tremor alone only trims within the
 * packet that carries the granule, so the page is read here as vorbisfile does, from the input
 * ring without consuming it: the blocksize of each packet completed on it from its mode number.
 * 0 when there is nothing to trim or the page cannot be read that way. */
static uint32_t start_trim(void)
{
    const codec_setup_info *ci = (const codec_setup_info *)vi.codec_setup;
    uint8_t h[27], lace[255], b;
    int modebits = 0;
    for (int v = ci->modes; v > 1; v >>= 1)
        modebits++;
    if (pg.seg != pg.nsegs || in_peek_ahead(0, h, 27) < 27 || memcmp(h, "OggS", 4) || h[4] ||
        (h[5] & 5) || (uint32_t)(h[14] | h[15] << 8 | h[16] << 16 | (uint32_t)h[17] << 24) != pg.serial ||
        in_peek_ahead(27, lace, h[26]) < h[26])
        return 0;                               /* continued or last page, other stream: left */
    int64_t granule = 0;
    for (int i = 7; i >= 0; i--)
        granule = granule << 8 | h[6 + i];
    int64_t sum = 0;
    uint32_t off = 27 + h[26], start = off;
    long prev = -1;
    for (int i = 0; i < h[26]; i++) {
        off += lace[i];
        if (lace[i] == 255)
            continue;
        if (off == start || in_peek_ahead(start, &b, 1) < 1 || (b & 1) ||
            (b >> 1 & ((1 << modebits) - 1)) >= ci->modes)
            return 0;                           /* not an audio packet */
        long bs = ci->blocksizes[ci->mode_param[b >> 1 & ((1 << modebits) - 1)].blockflag];
        if (prev >= 0)
            sum += (prev + bs) >> 2;
        prev = bs;
        start = off;
    }
    return granule >= 0 && sum > granule ? (uint32_t)(sum - granule) : 0;
}

int codec_open(void)
{
    static uint8_t ident[IDENT_MAX];
    pg_reset();
    packetno = 0;
    errors = eos = last = 0;
    vd = 0;
    vorbis_cache_hits = 0;
    int ilen = pg_packet(ident, sizeof ident);  /* identification header (30 bytes) */
    if (ilen < 30 || ident[0] != 1 || memcmp(ident + 1, "vorbis", 6))
        return -HP_ERR_FORMAT;
    if (ident[11] < 1 || ident[11] > 2)         /* audio_channels */
        return -HP_ERR_UNSUPPORTED;
    int len;
    if (ctl->comment_end_page) {                /* core A found where it ends (tags.c) */
        if (pg_skip_to(ctl->comment_end_page, ctl->comment_end_seg))
            return -HP_ERR_FORMAT;
    } else {
        len = pg_packet(pkt, 7);                /* comment header: only its start is read */
        if (len == 0 || pkt[0] != 3 || memcmp(pkt + 1, "vorbis", 6))
            return -HP_ERR_FORMAT;
    }
    len = pg_packet(pkt, sizeof pkt);           /* setup header (codebooks): must fit */
    if (len <= 0 || pkt[0] != 5)
        return -HP_ERR_FORMAT;
    if (cache.magic == CACHE_MAGIC && cache.ident_len == (uint32_t)ilen &&
        cache.setup_len == (uint32_t)len && !memcmp(cache.ident, ident, ilen) &&
        !memcmp(cache.setup, pkt, len) && cache.sum == cache_sum()) {
        vi = cache.vi;                          /* same headers: codebooks already decoded */
        heap_resume(cache.floor, cache.cur);
        packetno = 3;
        vorbis_cache_hits = 1;
    } else {
        cache.magic = 0;
        heap_reset();
        vorbis_info_init(&vi);
        vorbis_comment_init(&vc);
        if (header_in(ident, ilen, 1) || header_in(empty_comment, sizeof empty_comment, 0) ||
            header_in(pkt, len, 0))
            return -HP_ERR_FORMAT;
        if (vi.channels < 1 || vi.channels > 2)
            return -HP_ERR_UNSUPPORTED;
        if (ilen <= IDENT_MAX && len <= SETUP_MAX) {
            cache.floor = heap_keep();
            cache.cur = heap_cur;
            cache.vi = vi;
            cache.ident_len = (uint32_t)ilen;
            cache.setup_len = (uint32_t)len;
            memcpy(cache.ident, ident, ilen);
            memcpy(cache.setup, pkt, len);
            cache.sum = cache_sum();
            cache.magic = CACHE_MAGIC;
        }
    }
    vd = vorbis_dsp_create(&vi);
    if (!vd)
        return -HP_ERR_UNSUPPORTED;
    uint64_t g = ctl->end_granule_lo | (uint64_t)ctl->end_granule_hi << 32;
    ctl->rate = (uint32_t)vi.rate;
#if !defined(ECHO_NO_HWFFT) || defined(ECHO_HWTEST)
    ctl->fft_blocks = ctl->fft_soft = 0;
#endif
#ifdef ECHO_HWTEST
    for (uint32_t i = 0; i < sizeof ctl->hwf / 4; i++)
        ((volatile uint32_t *)&ctl->hwf)[i] = 0;
#endif
    ctl->channels = (uint32_t)vi.channels;
    ctl->bits = 16;
    ctl->frame_bytes = 4;
    ctl->total_frames = (uint32_t)g;
    ctl->error_info = heap_peak | vorbis_cache_hits << 31;
    resync = 0;
    vpos = gshift = 0;
    if (ctl->seek_ms && g && ctl->in_end != 0xffffffffu) {
        /* the page before the target (core A's), or a byte in proportion to the stream, then
         * the next page; synthesis restarted, output dropped until a granule gives the
         * position, played from there: up to a page before the target (§75) */
        uint64_t want = (uint64_t)ctl->seek_ms * vi.rate / 1000;
        uint32_t data = ctl->in_rd;
        uint32_t off = ctl->seek_offset ? ctl->seek_offset :   /* core A found the page */
                       data + (uint32_t)((uint64_t)(ctl->in_end - data) * want / g);
        if (want < g && ctl->in_end > data && !pg_jump(off)) {
            vorbis_dsp_restart(vd);
            packetno += 2;                      /* a discontinuity for Tremor's granule tracking */
            resync = 1;
        }
    }
    if (!resync) {
        gshift = start_trim();
        vpos = -gshift;                         /* codec_run() drops up to position 0 */
    }
    return 0;
}

/* One audio packet into Tremor: 0 at the end of the stream */
static int feed(void)
{
    ogg_buffer ob;
    ogg_reference ref;
    ogg_packet op;
    if (last)
        return 0;                               /* the stream ended: what follows is not read */
    int len = pg_packet(pkt, sizeof pkt);
    if (len == 0)
        return 0;
    if (len < 0) {                              /* too long for an audio packet: damaged */
        packetno++;
        return ++errors <= MAX_ERRORS;
    }
    packet(&ob, &ref, &op, pkt, len);
    if (pg_last_on_page()) {
        op.granulepos = pg.granule + gshift;    /* Tremor's count: no trimming of its own */
        op.e_o_s = (pg.flags & 4) != 0;
        last = op.e_o_s;
    }
    if (vorbis_dsp_synthesis(vd, &op, 1))
        return ++errors <= MAX_ERRORS;
    errors = 0;
    if (resync && op.granulepos != -1) {        /* the frames now available end at the granule */
        vpos = pg.granule - vorbis_dsp_pcmout(vd, 0, 0);
        resync = 0;
        ctl->start_frame = (uint32_t)(vpos > 0 ? vpos : 0);
    }
    return 1;
}

int codec_run(void)
{
    if (eos)
        return 2;
    int n = vorbis_dsp_pcmout(vd, 0, 0);
    if (n <= 0) {
        if (!feed())
            eos = 1;
        return 1;
    }
    if (resync) {                               /* before the first granule: dropped */
        vorbis_dsp_read(vd, n);
        return 1;
    }
    if (vpos < 0) {                             /* start trimming: up to position 0 */
        int d = -vpos < n ? (int)-vpos : n;
        vorbis_dsp_read(vd, d);
        vpos += d;
        return 1;
    }
    if (n > BLOCK_FRAMES)
        n = BLOCK_FRAMES;
    n = vorbis_dsp_pcmout(vd, pcm, n);
    vorbis_dsp_read(vd, n);
    vpos += n;
    while (out_space() < (uint32_t)n * 4)
        rt_wait();
    if (vi.channels == 2) {
        const uint32_t *w = (const uint32_t *)pcm;
        for (int i = 0; i < n; i++)
            out_word(w[i]);
    } else {
        for (int i = 0; i < n; i++)
            out_word((uint16_t)pcm[i] | (uint32_t)(uint16_t)pcm[i] << 16);
    }
    out_done((uint32_t)n);
    return 1;
}
