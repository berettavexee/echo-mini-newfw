/* Player (ETUDE §19 onwards): our codecs on core B (hifi_proto.h), the input ring fed from the
 * file by core A, core B's PCM copied into the DAC ring in SYSRAM. The stock codec modules and
 * their mailbox protocol (ETUDE §1.2) were dropped in 0.61. */
#include "player.h"
#include "audio.h"
#include "board.h"
#include "hifi.h"
#include "log.h"
#include "mp4.h"

enum { ST_IDLE, ST_LOAD, ST_OPEN, ST_PLAY, ST_END, ST_ERROR };

struct player_state player;

static const struct fat_file *file;
static struct fat_cursor cur;

static int fail(int err, const char *what)
{
    player.error = err;
    player.state = ST_ERROR;
    log_printf("player: %s (%d)", what, err);
    return err;
}

/* ---- our runtime and codecs (ETUDE §19) ---- */
#include "hifi_proto.h"

static uint32_t fed_since_ring, freed_since_ring;
static int mp4_on;                              /* the input is mp4.c's packet stream */

/* Input ring from the file, in pieces up to the ring end, as much as fits (MP4: the packet
 * stream, once 4 KiB of room are free) */
static void feed(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    uint32_t wr = c->in_wr, rd = c->in_rd;
    if (c->rewind_seq != c->rewind_ack) {       /* core B wants an earlier offset (it waits) */
        wr = rd = c->rewind_pos;
        c->in_rd = rd;
        c->in_wr = wr;
        c->rewind_ack = c->rewind_seq;
        fed_since_ring++;
    }
    if (mp4_on) {
        uint32_t room = HP_IN_SIZE - (wr - rd), nr;
        if (mp4_done() || room < 4096)
            return;
        uint32_t n = mp4_feed(wr, room, &nr);
        c->in_wr = wr + n;
        fed_since_ring += n;
        player.reads += nr;
        player.read_bytes += n;
        if (mp4_done())
            c->in_end = wr + n;
        return;
    }
    if ((int32_t)(rd - wr) > 0) {               /* core B skipped past what we wrote */
        wr = rd;
        if (wr > file->size)
            wr = file->size;
        c->in_wr = wr;
        fed_since_ring++;
    }
    for (int k = 0; k < 4 && wr < file->size; k++) {
        uint32_t room = HP_IN_SIZE - (wr - c->in_rd), o = wr % HP_IN_SIZE;
        uint32_t n = room < HP_IN_SIZE - o ? room : HP_IN_SIZE - o;
        if (n > 8192)
            n = 8192;
        if (n >= file->size - wr)
            n = file->size - wr;
        else if (wr % 512) {                    /* after a skip: back to sector alignment */
            if (n > 512 - wr % 512)
                n = 512 - wr % 512;
        }
        else if (room < 8192)
            break;                              /* wait for 8 KiB of room: fewer, longer reads */
        else
            n &= ~511u;
        if (!n)
            break;
        if (fat_pread(file, &cur, wr, (void *)(HP_IN_BASE + o), n) != (int)n) {
            log_printf("player: read error at %u (+%u)", wr, n);
            break;
        }
        wr += n;
        c->in_wr = wr;
        fed_since_ring += n;
        player.reads++;
        player.read_bytes += n;
    }
    if (wr == file->size)
        c->in_end = wr;
}

/* One round: positions both ways, input, doorbell when core B may have something to do */
/* PCM in core B's output ring not copied yet */
static uint32_t out_avail(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    return (c->out_wr + HP_OUT_SIZE - c->out_rd) % HP_OUT_SIZE;
}

/* One round: DMA position, core B's PCM into the DAC ring (SYSRAM), input, doorbell when core B
 * may have something to do (new input, or 4 KiB of its output ring freed) */
static int dac_switch;                          /* the next track's rate waits for the drain */

static void own_service(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    audio_update();
    uint32_t n = dac_switch ? 0 : out_avail(), room = audio_free();
    if (n > room)
        n = room;
    n &= ~3u;
    uint32_t rd = c->out_rd;
    while (n) {
        uint32_t k = HP_OUT_SIZE - rd < n ? HP_OUT_SIZE - rd : n;
        audio_write((const void *)(HP_OUT_BASE + rd), k);
        rd = rd + k == HP_OUT_SIZE ? 0 : rd + k;
        n -= k;
        freed_since_ring += k;
    }
    c->out_rd = rd;
    feed();
    if (fed_since_ring || freed_since_ring >= 4096) {
        fed_since_ring = freed_since_ring = 0;
        hifi_ring();
    }
}

/* Ogg: granule position of the last page (the stream length), from the end of the file;
 * 0 when none is found in the last 64 KiB */
static uint64_t ogg_end_granule(const struct fat_file *f)
{
    static uint8_t buf[8192 + 27];
    struct fat_cursor c = { 0, 0 };
    uint32_t end = f->size;
    for (int k = 0; k < 8 && end > 27; k++) {
        uint32_t start = end > 8192 ? end - 8192 : 0;
        uint32_t n = end - start + (end < f->size ? 27 : 0);   /* overlap: a header across */
        if (n > f->size - start)
            n = f->size - start;
        if (fat_pread(f, &c, start, buf, n) != (int)n)
            return 0;
        for (int i = (int)n - 27; i >= 0; i--) {
            const uint8_t *h = buf + i;
            if (h[0] != 'O' || h[1] != 'g' || h[2] != 'g' || h[3] != 'S' || h[4])
                continue;
            uint32_t lo = h[6] | h[7] << 8 | h[8] << 16 | (uint32_t)h[9] << 24;
            uint32_t hi = h[10] | h[11] << 8 | h[12] << 16 | (uint32_t)h[13] << 24;
            if (lo != 0xffffffffu || hi != 0xffffffffu)
                return lo | (uint64_t)hi << 32;
        }
        end = start;
    }
    return 0;
}

/* Ogg, for a seek: the granule rate (48 kHz for Opus, the rate of the Vorbis identification
 * header), 0 when unknown */
static uint32_t ogg_rate(const struct fat_file *f)
{
    static uint8_t b[512];
    struct fat_cursor c = { 0, 0 };
    uint32_t n = f->size < sizeof b ? f->size : sizeof b;
    if (fat_pread(f, &c, 0, b, n) != (int)n || n < 27 || b[0] != 'O' || b[3] != 'S' ||
        27u + b[26] + 16 > n)
        return 0;
    const uint8_t *p = b + 27 + b[26];
    if (!__builtin_memcmp(p, "OpusHead", 8))
        return 48000;
    if (p[0] == 1 && !__builtin_memcmp(p + 1, "vorbis", 6))
        return p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24;
    return 0;
}

/* Ogg, for a seek: offset of the last page whose granule is <= target (playback starts there,
 * up to a page before the target), 0 when none is found. The range [lo, hi) of the offsets to
 * read from shrinks around it, from 'start' (the page where the comment header ends, past a
 * cover picture: no granule there, 0 when unknown): a guess in proportion to the granules at its
 * ends (end: the last granule of the file, 0 when unknown), the middle whenever a guess did not
 * halve the range; each step reads from there to the first page with a granule. Exact: it stops
 * once the first such page after the best one is past the target. *reads: the reads done. */
static uint32_t ogg_page_before(const struct fat_file *f, uint64_t target, uint64_t end,
                                uint32_t start, uint32_t *reads)
{
    static uint8_t b[8192 + 27];
    struct fat_cursor c = { 0, 0 };
    uint32_t lo = start < f->size ? start : 0, hi = f->size, best = 0;
    uint64_t glo = 0, ghi = end;
    int guess = end > 0;
    for (int it = 0; it < 40 && hi > lo; it++) {
        uint32_t mid = lo + (hi - lo) / 2, span = hi - lo;
        if (guess && ghi > glo && target >= glo) {
            uint64_t d = (uint64_t)(hi - lo) * (target - glo) / (ghi - glo);
            d = d > 8192 ? d - 8192 : 0;        /* a little before: the page before the target */
            mid = lo + (uint32_t)d;
            if (mid > hi - 1)
                mid = hi - 1;
        }
        uint32_t at = mid, keep = 0;
        int found = 0;
        /* the first page after mid that ends a packet: pages are up to 64 KiB, read on */
        while (!found && at < f->size && at - mid < 72 * 1024) {
            uint32_t n = f->size - at < 8192 ? f->size - at : 8192;
            if (fat_pread(f, &c, at, b + keep, n) != (int)n)
                return best;
            (*reads)++;
            uint32_t total = keep + n, i;
            for (i = 0; i + 27 <= total; i++) {
                const uint8_t *h = b + i;
                if (h[0] != 'O' || h[1] != 'g' || h[2] != 'g' || h[3] != 'S' || h[4] || h[5] > 7)
                    continue;
                uint32_t lo32 = h[6] | h[7] << 8 | h[8] << 16 | (uint32_t)h[9] << 24;
                uint32_t hi32 = h[10] | h[11] << 8 | h[12] << 16 | (uint32_t)h[13] << 24;
                if (lo32 == 0xffffffffu && hi32 == 0xffffffffu)
                    continue;                   /* no packet ends on this page */
                uint32_t page = at - keep + i;
                uint64_t gp = lo32 | (uint64_t)hi32 << 32;
                if (gp <= target) {
                    best = page;
                    lo = page + 27;
                    glo = gp;
                } else {
                    hi = mid;
                    ghi = gp;                   /* that of a page after mid: an upper bound */
                }
                found = 1;
                break;
            }
            if (found)
                break;
            keep = total < 26 ? total : 26;     /* a header across two reads */
            for (uint32_t k = 0; k < keep; k++)
                b[k] = b[total - keep + k];
            at += n;
        }
        if (!found)
            hi = mid;
        guess = found && (hi - lo) * 2 <= span;
    }
    return best;
}

static void command(uint32_t cmd)
{
    volatile struct hp_ctl *c = HP_CTL;
    c->cmd = cmd;
    c->cmd_seq = c->cmd_seq + 1;
    hifi_ring();
}

/* Chain (PLAY_CHAIN): the DACs and the DMA keep running from one track to the next when the
 * sample rate and the frame size do not change; the DAC ring (743 ms at 44.1 kHz, 16 bits)
 * plays the end of the previous track while core B is restarted and opens the next one. A track
 * ends once all of core B's output is in the DAC ring, without waiting for it to drain. */
static struct { uint32_t rate, fb; int running; } dac;

/* The end of the last track heard while its last frames are in the DAC ring: from a chained
 * close (all its frames there, up to 'end') until the DMA is past them; player_heard keeps
 * describing it meanwhile. drain: the end of the queue followed from the main loop. */
static struct fat_file own_file;                /* the track open (non-blocking form) */
struct player_heard player_heard;
static struct { int on; uint32_t end, frames, fb; } tail;
static struct { int on; uint32_t end, t0; } drain;

static void heard_publish(void)                 /* the track opened last is heard from now on */
{
    player_heard.rate = player.rate;
    player_heard.bps = player.bps;
    player_heard.length_ms = player.length_ms;
    player_heard.time_ms = player.time_ms;
    player_heard.gen++;
}

static void heard_update(void)
{
    if (tail.on) {
        int32_t left = (int32_t)(tail.end - audio.rd);
        uint32_t q = left > 0 && audio.on ? (uint32_t)left / tail.fb : 0;
        uint32_t fr = tail.frames > q ? tail.frames - q : 0;
        player_heard.time_ms = player_heard.rate ?
                               (uint32_t)((uint64_t)fr * 1000 / player_heard.rate) : 0;
        if (q)
            return;
        tail.on = 0;                            /* past the last frame */
        if (!player_is_open() || dac_switch)
            return;                             /* (a rate change: published after it) */
        log_printf("player: now hearing %s", own_file.name);
        heard_publish();
        return;
    }
    if (player_is_open())
        player_heard.time_ms = player.time_ms;
}

void player_drop_tail(void)
{
    tail.on = drain.on = 0;
}

/* ---- core B clock (step 5g, ETUDE §24) ----
 * Load = time core B is awake over 250 ms windows of TIMER1 (its sleeps are timed by its
 * runtime). Automatic mode: the DAC ring (1.1 s at 44.1 kHz) absorbs the peaks of load, so the
 * clock goes up only when that ring really goes down (below 3/4 and falling by more than 4 KiB in
 * a window) or when the load stays above 85 % for two windows: then at least to the slowest level
 * that would keep the load at 75 % (60 % up to 0.42). Down one level after 1 s in which a lower
 * level would do. The top level from the start of a track (load, open) until the DAC ring is full (0.36 went up on
 * every peak: Opus switched 29 times in 2 min between 133 and 200 MHz, ETUDE §24). */
static const uint8_t gov_div[] = { 8, 7, 6, 5, 4, 3, 2 };   /* 50 57 67 80 100 133 200 MHz */
#define GOV_LEVELS ((int)sizeof gov_div)
int player_clock;                               /* 0: automatic, else a fixed divider */
static struct {
    int level, boost, low, hot;
    uint32_t t0, t_last, idle_last;
    int32_t fill_last;
    uint32_t windows, sum_mhz10, max_mhz10, max_load, changes, ms[GOV_LEVELS];
} gov;

static uint32_t b_idle(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    for (;;) {
        uint32_t s1 = c->sleep_since, i = c->idle_ticks, s2 = c->sleep_since;
        if (s1 == s2)
            return i + (s1 ? timer24() - (s1 & ~1u) : 0);
    }
}

static void gov_set(int level)
{
    if (level != gov.level) {
        gov.level = level;
        gov.changes++;
        hifi_set_div(gov_div[level]);
    }
}

/* At the start of a track, before core B is loaded or restarted */
static void gov_start(void)
{
    uint32_t keep = gov.level;
    for (unsigned i = 0; i < sizeof gov / sizeof(uint32_t); i++)
        ((uint32_t *)&gov)[i] = 0;
    gov.level = (int)keep;
    if (player_clock) {
        hifi_set_div((uint32_t)player_clock);
        for (int i = 0; i < GOV_LEVELS; i++)
            if (gov_div[i] == player_clock)
                gov.level = i;
    } else {
        gov.level = -1;
        gov_set(GOV_LEVELS - 1);
        gov.changes = 0;
        gov.boost = 1;
    }
    gov.t0 = gov.t_last = timer24();
}

/* Once core B runs our runtime: the window starts here */
static void gov_begin(void)
{
    gov.t_last = timer24();
    gov.idle_last = b_idle();
    gov.fill_last = audio_fill();
}

static void gov_window(void)
{
    uint32_t now = timer24(), el = now - gov.t_last;
    if (el < 6000000)                           /* 250 ms */
        return;
    uint32_t idle = b_idle(), di = idle - gov.idle_last;
    uint32_t busy = el > di ? el - di : 0;
    uint32_t load = (uint32_t)((uint64_t)busy * 1000 / el);     /* per mille */
    uint32_t mhz10 = hifi_hz / 100000 * load / 1000;            /* tenths of MHz used */
    gov.windows++;
    gov.sum_mhz10 += mhz10;
    player.b_load = load;
    player.b_mhz10 = mhz10;
    if (mhz10 > gov.max_mhz10)
        gov.max_mhz10 = mhz10;
    if (load > gov.max_load)
        gov.max_load = load;
    if (gov.level >= 0)
        gov.ms[gov.level] += el / 24000;
    int32_t fill = audio_fill();
    if (!player_clock) {
        if (gov.boost) {
            if (fill >= (int32_t)audio.ring_size - 16384 || now - gov.t0 > 72000000)
                gov.boost = 0;                  /* DAC ring full (or 3 s): end of the boost */
        } else {
            uint32_t need10 = mhz10 * 4 / 3;    /* for a 75 % load */
            int want = GOV_LEVELS - 1;
            for (int i = 0; i < GOV_LEVELS; i++)
                if (4000 / gov_div[i] >= need10) {
                    want = i;
                    break;
                }
            gov.hot = load > 850 ? gov.hot + 1 : 0;
            int falling = fill < (int32_t)(audio.ring_size / 4 * 3) && fill + 4096 < gov.fill_last;
            if ((falling || gov.hot >= 2) && gov.level < GOV_LEVELS - 1) {
                gov_set(want > gov.level ? want : gov.level + 1);
                gov.low = gov.hot = 0;
            } else if (want < gov.level) {
                if (++gov.low >= 4) {           /* 1 s below: one level down */
                    gov_set(gov.level - 1);
                    gov.low = 0;
                }
            } else
                gov.low = 0;
        }
    }
    gov.fill_last = fill;
    gov.t_last = now;
    gov.idle_last = idle;
}

static void gov_report(const char *name)
{
    uint32_t mean = gov.windows ? gov.sum_mhz10 / gov.windows : 0;
    log_printf("clock: %s: core B %s, used mean %u.%u MHz, max %u.%u MHz (250 ms), max load %u.%u %%; "
               "ms at 50/57/67/80/100/133/200 MHz: %u %u %u %u %u %u %u; %u changes", name,
               player_clock ? "fixed" : "automatic", mean / 10, mean % 10, gov.max_mhz10 / 10,
               gov.max_mhz10 % 10, gov.max_load / 10, gov.max_load % 10, gov.ms[0], gov.ms[1],
               gov.ms[2], gov.ms[3], gov.ms[4], gov.ms[5], gov.ms[6], gov.changes);
}

/* The longest a drain may take: the DAC ring at the rate playing (1.1 s at 44.1 kHz, 6.1 s at
 * 8 kHz in 16 bits), plus 1 s (0.79, ETUDE §60: a fixed 2 s cut the end of the low rates) */
static uint32_t drain_limit_us(void)
{
    uint32_t bps = audio.rate * audio.frame_bytes;
    return (bps ? (uint32_t)((uint64_t)audio.ring_size * 1000000 / bps) : 2000000) + 1000000;
}

static void chain_done(void)
{
    dac.running = 0;
    if (tail.on) {                              /* heard to its end (or the wait gave up) */
        tail.on = 0;
        player_heard.time_ms = player_heard.rate ?
                               (uint32_t)((uint64_t)tail.frames * 1000 / player_heard.rate) : 0;
    }
    drain.on = 0;
}

void player_chain_end(void)
{
    if (dac.running) {
        static const uint32_t zero[256];
        uint32_t t0 = ticks(), end = audio.wr;  /* the end of the last track's frames */
        uint32_t lim = drain_limit_us();
        while (audio_free() < sizeof zero && ticks() - t0 < lim) {
            wdt_kick();
            audio_update();
        }
        audio_write(zero, sizeof zero);         /* 1 KiB of silence: the margin of the cut */
        while ((int32_t)(audio.rd - end) < 0 && ticks() - t0 < lim) {
            wdt_kick();                         /* until the DMA is past the last frame */
            audio_update();
        }
    }
    chain_done();
}

/* One round of a drain followed from the main loop: silence written behind the last frame as
 * the DMA frees room (the loop may come back late: the DMA never reaches stale data), 1 once
 * the DMA is past the last frame (or after the ring's duration and 1 s of playing, a pause not
 * counted) */
static int drain_step(void)
{
    static const uint32_t zero[256];
    audio_update();
    if (!drain.on) {
        drain.on = 1;
        drain.end = audio.wr;                   /* the end of the last track's frames */
        drain.t0 = ticks();
    }
    while (audio_free() >= sizeof zero)
        audio_write(zero, sizeof zero);
    heard_update();
    if (audio_paused)
        drain.t0 = ticks();                     /* a pause holds the DMA: no time-out then */
    return (int32_t)(audio.rd - drain.end) >= 0 || ticks() - drain.t0 >= drain_limit_us();
}

int player_drain_service(void)
{
    if (!dac.running)
        return 1;
    if (!drain_step())
        return 0;
    chain_done();
    return 1;
}

/* ---- non-blocking playback (step 6): open, then service from the main loop, then close ---- */
static struct {
    int flags, warm, kept, changed, full, open;
    uint32_t t0, t_load, t_ready, t_open, t_dac, t_first, t_play, ur0, lost0, i2s0, fb;
    uint32_t na_frames, na_t, na_in;    /* the last change of the frames decoded: when, input */
    int32_t fill0;
} ps;

static uint32_t hint_page, hint_seg, hint_seek;

void player_seek_hint(uint32_t ms)
{
    hint_seek = ms;
}

void player_ogg_hint(uint32_t page, uint32_t seg)
{
    hint_page = page;
    hint_seg = seg;
}

int player_open(const struct fat_file *f, uint32_t codec, int flags)
{
    volatile struct hp_ctl *c = HP_CTL;
    int r;
    uint32_t cpage = hint_page, cseg = hint_seg, seek = hint_seek;     /* for this open only */
    hint_page = hint_seg = hint_seek = 0;

    for (unsigned i = 0; i < sizeof player / sizeof(uint32_t); i++)
        ((uint32_t *)&player)[i] = 0;
    for (unsigned i = 0; i < sizeof ps / sizeof(uint32_t); i++)
        ((uint32_t *)&ps)[i] = 0;
    own_file = *f;
    file = &own_file;
    ps.flags = flags;
    ps.t0 = ticks();
    cur.index = cur.cluster = 0;
    fed_since_ring = freed_since_ring = 0;
    player.state = ST_LOAD;
    ps.kept = (flags & PLAY_CHAIN) && dac.running && audio.on;
    drain.on = 0;
    ps.fill0 = ps.kept ? audio_fill() : 0;      /* what the DAC ring holds at the switch */
    ps.ur0 = audio.ur_all;                      /* the counters of this track by difference */
    ps.lost0 = audio.lost_all;
    ps.i2s0 = audio.i2s_all;
    log_printf("player: %s, %u bytes, our codec %u", f->name, f->size, codec);
    mp4_on = codec == HP_CODEC_AAC || codec == HP_CODEC_ALAC;
    if (mp4_on) {                               /* the container read here, the seek done here */
        struct mp4_track t;
        r = mp4_open(file, seek, &t);         /* own_file: f may be the caller's copy */
        if (r)
            return fail(r == MP4_EREAD ? HIFI_EREAD : r == MP4_EFORMAT ? HIFI_EPROTO : HIFI_EIMG,
                        r == MP4_EUNSUPPORTED ? "MP4: neither AAC-LC nor ALAC as we play them" :
                        "MP4: container not readable");
        seek = 0;
        codec = t.codec;                        /* an M4A: AAC or ALAC, the container says */
    }
    player.codec = codec;
    gov_start();                                /* automatic: top level for the load and open */
    uint32_t img = codec == HP_CODEC_AIFF ? HP_CODEC_WAV : codec;  /* one image for PCM */
    if (hifi_own_loaded() == img && !hifi_restart_own())
        ps.warm = 1;                            /* the image is still in the HIGHRAM */
    else if ((r = hifi_load_own(img)))
        return fail(r, "our codec is not in the firmware image");
    ps.t_load = ticks();
    if ((r = hifi_start_own()))
        return fail(r, "our core B runtime did not start");
    ps.t_ready = ticks();
    gov_begin();
    if (ps.kept)
        audio_update();
    c->in_end = mp4_on ? 0xffffffffu : f->size; /* known from the start (MP3 length estimate) */
    c->comment_end_page = codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS ? cpage : 0;
    c->comment_end_seg = cseg;
    c->seek_ms = seek;
    c->seek_offset = 0;
    if (c->comment_end_page)
        log_printf("player: comment header ends at page %u, segment %u", c->comment_end_page,
                   c->comment_end_seg);
    uint64_t end_g = 0;
    if (codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS) {
        end_g = ogg_end_granule(file);
        c->end_granule_lo = (uint32_t)end_g;
        c->end_granule_hi = (uint32_t)(end_g >> 32);
        cur.index = cur.cluster = 0;
        log_printf("player: last Ogg granule %u", (uint32_t)end_g);
    }
    if (seek && (codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS)) {
        /* the page before the target: playback starts up to a page early, nothing decoded for
         * nothing (§75) */
        uint32_t rate = ogg_rate(f), reads = 0, t_seek = ticks();
        uint64_t g = (uint64_t)seek * rate / 1000;
        if (rate && g)
            c->seek_offset = ogg_page_before(f, g, end_g, cpage, &reads);
        cur.index = cur.cluster = 0;
        log_printf("player: start at %u ms: Ogg page at %u, %u reads in %u us", seek,
                   c->seek_offset, reads, ticks() - t_seek);
    } else if (seek)
        log_printf("player: start at %u ms", seek);
    feed();
    command(HP_CMD_OPEN);
    player.state = ST_OPEN;
    for (uint32_t ts = ticks(); c->state != HP_FORMAT;) {
        wdt_kick();
        if (ps.kept)
            audio_update();
        feed();
        if (fed_since_ring) {
            fed_since_ring = 0;
            hifi_ring();
        }
        if (c->state == HP_ERROR || ticks() - ts > 3000000) {
            log_printf("player: open failed: state %u error %u, fault pc %x lr %x cfsr %x, in %u/%u",
                       c->state, c->error, c->fault_pc, c->fault_lr, c->fault_cfsr, c->in_rd, c->in_wr);
            hifi_stop();
            return fail(HIFI_EPROTO, "our codec could not open the file");
        }
    }
    ps.t_open = ticks();
    player.rate = c->rate;
    player.channels = c->channels;
    player.bps = c->bits;
    player.length_ms = player.rate ? (uint32_t)((uint64_t)c->total_frames * 1000 / player.rate) : 0;
    log_printf("player: format: %u Hz, %u ch, %u bits, %u-byte frames, %u ms, after %u bytes of "
               "input%s", player.rate, player.channels, player.bps, c->frame_bytes, player.length_ms,
               c->in_rd, c->codec == HP_CODEC_VORBIS && (c->error_info >> 31) ?
               ", codebooks from the cache" : "");
    /* core B decodes into its output ring while the DACs are set up */
    command(HP_CMD_PLAY);
    ps.fb = c->frame_bytes;
    if (ps.kept && (dac.rate != player.rate || dac.fb != ps.fb)) {
        dac_switch = 1;                         /* another rate or frame size: the DAC ring */
        drain.on = 0;                           /* plays out first, followed by player_service */
        log_printf("player: %s at %u Hz waits for %u ms of the last track at %u Hz", f->name,
                   player.rate, (uint32_t)((uint64_t)(uint32_t)audio_fill() / dac.fb * 1000 /
                                           (dac.rate ? dac.rate : 1)), dac.rate);
    }
    if (!ps.kept && !ps.changed) {
        audio_start(player.rate, ps.fb == 4);
        audio_update();
        audio.wr = (audio.rd + 8192) & ~7u;     /* 8 KiB of the zeroed ring, then core B's PCM */
        log_printf("player: core B had %u bytes ready when the DMA started", out_avail());
    }
    if (!ps.kept)
        tail.on = 0;                            /* the DACs started again: nothing of it left */
    if (tail.on && (int32_t)(tail.end - audio.rd) <= 0)
        tail.on = 0;                            /* played out meanwhile (a slow refusal before) */
    if (dac_switch)
        ;                                       /* published after the switch */
    else if (tail.on)
        log_printf("player: %s queued behind %u ms of the last track", f->name,
                   (uint32_t)((uint64_t)(uint32_t)(tail.end - audio.rd) / tail.fb * 1000 /
                              (player_heard.rate ? player_heard.rate : 1)));
    else
        heard_publish();
    ps.t_dac = ticks();
    if (!dac_switch) {                          /* else the DACs keep the last track's rate */
        dac.rate = player.rate;
        dac.fb = ps.fb;
    }
    dac.running = 1;
    ramp_hook = own_service;
    player.state = ST_PLAY;
    player.b_load = player.b_mhz10 = 0;
    ps.t_play = ticks();
    ps.open = 1;
    return 0;
}

#define NOAUDIO_US    5000000u
#define NOAUDIO_BYTES (1u << 20)

/* One round of an open track: 0 while it plays, 1 at its end (all of core B's output in the DAC
 * ring with PLAY_CHAIN, the DAC ring drained otherwise), < 0 on an error */
int player_service(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    if (!ps.open)
        return 1;
    wdt_kick();
    own_service();
    gov_window();
    if (dac_switch) {
        if (!drain_step())
            return 0;                           /* the last track still plays */
        chain_done();
        dac_switch = 0;
        uint32_t t_sw = ticks();
        ps.kept = 0;                            /* only the rate-dependent part */
        if (!audio_change_rate(player.rate, ps.fb == 4)) {
            audio_update();
            audio.wr = (audio.rd + 1024) & ~7u; /* core B's frames are ready: 1 KiB ahead */
            ps.changed = 1;
        } else {
            audio_start(player.rate, ps.fb == 4);
            audio_update();
            audio.wr = (audio.rd + 8192) & ~7u;
        }
        dac.rate = player.rate;
        dac.fb = ps.fb;
        dac.running = 1;
        ps.t_dac = ps.t_open + (ticks() - t_sw); /* the log: the change itself, not the wait */
        ps.t_play = ticks();
        log_printf("player: now hearing %s", own_file.name);
        heard_publish();
        own_service();
    }
    if (!ps.t_first && c->frames) {
        ps.t_first = ticks();                   /* first PCM of this track copied */
        log_printf("chain: %s: core B %s %u us, runtime %u us, open %u us (%u bytes), "
                   "DAC %s %u us, first PCM %u us after the start; DAC ring at the "
                   "switch %d bytes (%u ms), underruns during it %u (%u bytes)",
                   own_file.name, ps.warm ? "restarted (same codec) in" : "loaded in",
                   ps.t_load - ps.t0, ps.t_ready - ps.t_load, ps.t_open - ps.t_ready, c->in_rd,
                   ps.kept ? "kept" : ps.changed ? "rate changed in" : "started in",
                   ps.t_dac - ps.t_open, ps.t_first - ps.t0, ps.fill0,
                   ps.fill0 > 0 ? (uint32_t)((uint64_t)ps.fill0 * 1000 / ps.fb / player.rate) : 0,
                   audio.ur_all - ps.ur0, audio.lost_all - ps.lost0);
    }
    if (!ps.full && audio_fill() >= (int32_t)(audio.ring_size - 8192)) {
        ps.full = 1;                            /* from here, the steady-state minimum fill */
        log_printf("player: DAC ring full after %u ms (core B ring %u bytes)",
                   (ticks() - ps.t_play) / 1000, out_avail());
        if (!ps.kept)
            audio_reset_stats();
        audio.min_fill = 0x7fffffff;            /* a chained track too: its own minimum */
    }
    /* frames heard: written by core B minus what is still in the two rings */
    uint32_t queued = (out_avail() + (uint32_t)(audio_fill() > 0 ? audio_fill() : 0)) / ps.fb;
    player.frames = c->start_frame + (c->frames > queued ? c->frames - queued : 0);
    player.time_ms = player.rate ? (uint32_t)((uint64_t)player.frames * 1000 / player.rate) : 0;
    heard_update();
    if (c->state == HP_ERROR) {
        log_printf("player: core B error %u, fault pc %x lr %x cfsr %x, at %u MHz", c->error,
                   c->fault_pc, c->fault_lr, c->fault_cfsr, hifi_hz / 1000000);
        player.error = HIFI_EPROTO;
        return player.error;
    }
    if (c->state == HP_END && !out_avail() &&
        ((ps.flags & PLAY_CHAIN) || audio_fill() <= 4096)) {
        player.state = ST_END;
        return 1;
    }
    /* A damaged file (zeros, garbage): NOAUDIO_US without a frame while core B reads on (not in
     * a pause, nor while the DACs wait for the last track: core B waits for room then) */
    if (c->frames != ps.na_frames || !ps.na_t) {
        ps.na_frames = c->frames;
        ps.na_t = ticks() | 1;
        ps.na_in = c->in_rd;
    } else if (!dac_switch && ticks() - ps.na_t > NOAUDIO_US &&
               c->in_rd - ps.na_in > NOAUDIO_BYTES) {
        log_printf("player: no audio decoded for %u ms over %u bytes of input (at %u of %u): "
                   "damaged file", (ticks() - ps.na_t) / 1000, c->in_rd - ps.na_in, c->in_rd,
                   own_file.size);
        player.error = PLAYER_ENOAUDIO;
        return player.error;
    }
    return 0;
}

/* The end of an open track: 'stopped' by the user, or at its end (or after an error) */
void player_close(int stopped)
{
    volatile struct hp_ctl *c = HP_CTL;
    if (!ps.open)
        return;
    ps.open = 0;
    ramp_hook = 0;
    log_printf("player: %s after %u frames, %u ms; input %u bytes in %u reads; core B woke %u "
               "times, %u loops; DAC ring %u KiB: underruns %u (%u bytes), i2s %u, min fill %d%s",
               stopped ? "stopped" : (player.error ? "failed" : "finished"), c->frames,
               player.time_ms, player.read_bytes, player.reads, c->wakeups, c->work_loops,
               audio.ring_size / 1024, audio.ur_all - ps.ur0, audio.lost_all - ps.lost0,
               audio.i2s_all - ps.i2s0,
               audio.min_fill, ps.full ? " (since full)" : " (never full)");
    gov_report(own_file.name);
    if (c->synth_gran || c->synth_soft)
        log_printf("synth: %s: %u granules on SYNTH, %u timeouts (software for the rest)",
                   own_file.name, c->synth_gran, c->synth_soft);
#if !defined(ECHO_NO_HWFFT) || defined(ECHO_HWTEST)
    if (c->fft_blocks || c->fft_soft)
        log_printf("fft: %s: %u IMDCT blocks on HIFIACC, %u in software after a timeout",
                   own_file.name, c->fft_blocks, c->fft_soft);
#endif
#ifdef ECHO_HWTEST
    if (c->hwf.magic == HWT_MAGIC && c->hwf.blocks)  /* test build: FFT against Tremor (§74) */
        log_printf("hwf: %s: %u IMDCT blocks (%u short), %u samples: %u exact, %u within 1, "
                   "max %u LSB; hw %u us, Tremor %u us in all; %u timeouts", own_file.name,
                   c->hwf.blocks, c->hwf.short_blocks, c->hwf.samples, c->hwf.exact,
                   c->hwf.within1, c->hwf.max, c->hwf.hw_ticks / 24, c->hwf.sw_ticks / 24,
                   c->hwf.timeouts);
#endif
    if (c->hacc_sub || c->hacc_soft)
        log_printf("hifiacc: %s: %u LPC subframes on HIFIACC, %u in software after a timeout",
                   own_file.name, c->hacc_sub, c->hacc_soft);
#ifdef ECHO_HWSYN
    if (c->hws.magic == HWT_MAGIC && c->hws.p_cmp)   /* test build: SYNTH plays (§71) */
        log_printf("hwp: %s: %u samples compared: %u exact, %u within 1, max %u; synth %u us "
                   "(%u polled, %u timeouts)", own_file.name, c->hws.p_cmp, c->hws.p_exact,
                   c->hws.p_within1, c->hws.p_max, c->hws.p_hw_ticks / 24, c->hws.p_polled,
                   c->hws.p_timeouts);
#endif
#ifdef ECHO_HWTEST
    if (c->hwt.magic == HWT_MAGIC) {            /* test build: HIFIACC against dr_flac (§66) */
        log_printf("hwt: %s: %u LPC subframes: %u exact, %u differ, %u timeouts, %u skipped; "
                   "wide %u (%u differ); max order %u shift %u n %u; hw %u us, sw %u us; "
                   "orders differing %x", own_file.name, c->hwt.sub, c->hwt.exact, c->hwt.diff,
                   c->hwt.timeout, c->hwt.skipped, c->hwt.wide, c->hwt.wide_diff,
                   c->hwt.max_order, c->hwt.max_shift, c->hwt.max_n, c->hwt.hw_ticks / 24,
                   c->hwt.sw_ticks / 24, c->hwt.diff_orders);
        if (c->hwt.diff)
            log_printf("hwt: first difference: order %u shift %u precision %u bps %u n %u at %u: "
                       "sw %d hw %d", c->hwt.f_order, c->hwt.f_shift, c->hwt.f_prec,
                       c->hwt.f_bps, c->hwt.f_n, c->hwt.f_idx, c->hwt.f_sw, c->hwt.f_hw);
        log_printf("hwt: split %u (longest pass %u)", c->hwt.split, c->hwt.max_seg);
        for (int o = 0; o < 32; o++)
            if (c->hwt.ok_o[o] || c->hwt.diff_o[o])
                log_printf("hwt: order %d: %u exact, %u differ", o + 1, c->hwt.ok_o[o],
                           c->hwt.diff_o[o]);
        if (c->hwt.o1_shift)
            log_printf("hwt: order 1: coef %d shift %d; x %d %d %d %d %d %d; sw %d %d %d %d %d %d; "
                       "hw %d %d %d %d %d %d", c->hwt.o1_coef, c->hwt.o1_shift,
                       c->hwt.o1_x[0], c->hwt.o1_x[1], c->hwt.o1_x[2], c->hwt.o1_x[3],
                       c->hwt.o1_x[4], c->hwt.o1_x[5], c->hwt.o1_sw[0], c->hwt.o1_sw[1],
                       c->hwt.o1_sw[2], c->hwt.o1_sw[3], c->hwt.o1_sw[4], c->hwt.o1_sw[5],
                       c->hwt.o1_hw[0], c->hwt.o1_hw[1], c->hwt.o1_hw[2], c->hwt.o1_hw[3],
                       c->hwt.o1_hw[4], c->hwt.o1_hw[5]);
        c->hwt.magic = 0;
    }
    if (c->hws.magic == HWT_MAGIC && c->hws.slots) {    /* test build: SYNTH against pvmp3dec */
        log_printf("hws: %s: trial %u shift %d: %u slots (%u polled, %u timeouts); direct: %u "
                   "exact, %u within 1, %u within 4, max %u; reversed: %u exact, %u within 4",
                   own_file.name, c->hws.trial, c->hws.shift, c->hws.slots, c->hws.polled,
                   c->hws.timeouts, c->hws.exact, c->hws.within1, c->hws.within4, c->hws.max_err,
                   c->hws.r_exact, c->hws.r_within4);
        log_printf("hws: sum |sw| %u, |hw| %u (x1000), dot %d (x1e6); first: x %d %d %d %d, "
                   "sw %d %d %d %d, hw %d %d %d %d; +0x80c %x then %x",
                   (uint32_t)(c->hws.sum_sw / 1000), (uint32_t)(c->hws.sum_hw / 1000),
                   (int32_t)(c->hws.dot / 1000000), c->hws.x[0], c->hws.x[1], c->hws.x[2],
                   c->hws.x[3], c->hws.sw[0], c->hws.sw[1], c->hws.sw[2], c->hws.sw[3],
                   c->hws.hw[0], c->hws.hw[1], c->hws.hw[2], c->hws.hw[3], c->hws.st_before,
                   c->hws.st_after);
        if (c->hws.syn_timed)
            log_printf("hws: synth time: hw %u us, sw %u us over %u calls (18 slots each)",
                       c->hws.syn_hw_ticks / 24, c->hws.syn_sw_ticks / 24, c->hws.syn_timed);
        if (c->hws.im_calls) {
            log_printf("hwi: mode %u shift %d: %u transforms (%u polled, %u timeouts); hw %u us, "
                       "mdct_18 %u us; sum |ref| %u, |hw| %u (x1000); dot %d (x1e6)",
                       c->hws.im_mode, c->hws.im_shift, c->hws.im_calls, c->hws.im_polled,
                       c->hws.im_timeouts, c->hws.im_hw_ticks / 24, c->hws.im_sw_ticks / 24,
                       (uint32_t)(c->hws.im_sum_ref / 1000), (uint32_t)(c->hws.im_sum_hw / 1000),
                       (int32_t)(c->hws.im_dot / 1000000));
            for (int e = 0; e < 17; e++)
                if (c->hws.im_within1[e] > c->hws.im_calls * 36 / 20)
                    log_printf("hwi: scale 2^%d: %u exact, %u within 1 (of %u)", e - 8,
                               c->hws.im_exact[e], c->hws.im_within1[e], c->hws.im_calls * 36);
            log_printf("hwi: first: x %d %d %d, ref %d %d %d, hw %d %d %d", c->hws.im_x[0],
                       c->hws.im_x[1], c->hws.im_x[2], c->hws.im_ref[0], c->hws.im_ref[1],
                       c->hws.im_ref[2], c->hws.im_hw[0], c->hws.im_hw[1], c->hws.im_hw[2]);
        }
        c->hws.slots = 0;
        c->hws.trial++;                         /* the next track tries the next shift */
    }
#endif
    command(HP_CMD_STOP);
    hifi_stop();
    if (stopped || player.error || !(ps.flags & PLAY_CHAIN))
        dac.running = 0;
    dac_switch = 0;                             /* the DACs still at the last track's rate */
    tail.on = 0;
    if (!stopped && !player.error && player.state == ST_END && dac.running && ps.fb) {
        tail.on = 1;                            /* its end still plays: followed from here */
        tail.end = audio.wr;
        tail.frames = c->start_frame + c->frames;
        tail.fb = ps.fb;
        heard_update();
    }
}

int player_is_open(void)
{
    return ps.open;
}
