/* Player (ETUDE §19 onwards): our codecs on core B (hifi_proto.h), the input ring fed from the
 * file by core A, core B's PCM copied into the DAC ring in SYSRAM. The stock codec modules and
 * their mailbox protocol (ETUDE §1.2) were dropped in 0.61. */
#include "perf.h"
#include "player.h"
#include "audio.h"
#include "board.h"
#include "bytes.h"
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
#include "crash.h"

/* Core B's error (enum hp_err) as a player error; its reason into player.why */
static int b_error(volatile struct hp_ctl *c)
{
    player.why = c->why;
    return c->error == HP_ERR_FORMAT ? PLAYER_EFORMAT :
           c->error == HP_ERR_UNSUPPORTED ? PLAYER_EUNSUPPORTED :
           c->error == HP_ERR_DECODE ? PLAYER_EDECODE : PLAYER_EFAULT;
}

static uint32_t fed_since_ring, freed_since_ring;
static int mp4_on;                              /* the input is mp4.c's packet stream */

/* Input ring from the file (version 3, ETUDE §105): the stream in segments; feed_pos is the
 * stream offset of the next byte written. Core B's requests (another offset: back, or forward
 * past what is written) start a new segment there. MP4: the packet stream, once 4 KiB of room
 * are free (no request then). */
static uint32_t feed_pos, feed_ack;

static void feed_start(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    feed_pos = feed_ack = 0;
    c->in_wr = 0;
    c->seg[0].seq = 0;
    c->seg[0].ring0 = 0;
    c->seg[0].off0 = 0;
    c->seg[0].prev_end = 0;
    c->seg_wr = 1;
}


/* A read failing again and again at one place (a card pulled out: until the detect switch is
 * seen, ETUDE §128): one line, then how many times */
static uint32_t rerr_pos, rerr_n;

static void read_errors_said(void)
{
    if (rerr_n > 1)
        log_printf("player: read error at %u: %u times", rerr_pos, rerr_n);
    rerr_n = 0;
}

static void feed(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    uint32_t wr = c->in_wr;
    if (c->req_seq != feed_ack && c->seg_wr - c->seg_rd < HP_SEGS) {
        feed_ack = c->req_seq;                  /* core B wants another offset (it waits) */
        uint32_t pos = c->req_pos;
        feed_pos = pos < file->size ? pos : file->size;
        volatile struct hp_seg *g = &c->seg[c->seg_wr % HP_SEGS];
        g->seq = feed_ack;
        g->prev_end = wr;
        wr += (feed_pos - wr) % 512;            /* the same place in a sector as in the file */
        c->in_wr = wr;
        g->ring0 = wr;
        g->off0 = feed_pos;
        c->seg_wr = c->seg_wr + 1;
        fed_since_ring++;
    }
    if (mp4_on) {
        uint32_t room = HP_IN_SIZE - (wr - c->rb_rd), nr;
        if (mp4_done() || room < 4096)
            return;
        uint32_t n = mp4_feed(wr, room, &nr);
        c->in_wr = wr + n;
        feed_pos += n;
        fed_since_ring += n;
        player.reads += nr;
        player.read_bytes += n;
        if (mp4_done())
            c->in_end = feed_pos;
        return;
    }
    for (int k = 0; k < 4 && feed_pos < file->size; k++) {
        uint32_t room = HP_IN_SIZE - (wr - c->rb_rd), o = wr % HP_IN_SIZE;
        uint32_t n = room < HP_IN_SIZE - o ? room : HP_IN_SIZE - o;
        if (n > 8192)
            n = 8192;
        if (n >= file->size - feed_pos)
            n = file->size - feed_pos;
        else if (feed_pos % 512) {              /* after a jump: back to sector alignment */
            if (n > 512 - feed_pos % 512)
                n = 512 - feed_pos % 512;
        }
        else if (room < 8192)
            break;                              /* wait for 8 KiB of room: fewer, longer reads */
        else
            n &= ~511u;
        if (!n)
            break;
        if (fat_pread(file, &cur, feed_pos, (void *)(HP_IN_BASE + o), n) != (int)n) {
            if (!rerr_n || rerr_pos != feed_pos)        /* once a place: a card pulled out */
                log_printf("player: read error at %u (+%u)", feed_pos, n);  /* fails at each */
            if (rerr_pos != feed_pos)                   /* round until it is seen gone */
                rerr_n = 0;
            rerr_pos = feed_pos;
            rerr_n++;
            break;
        }
        read_errors_said();
        wr += n;
        feed_pos += n;
        c->in_wr = wr;
        fed_since_ring += n;
        player.reads++;
        player.read_bytes += n;
    }
}

/* One round: positions both ways, input, doorbell when core B may have something to do */
/* PCM in core B's output ring not copied yet */
static uint32_t out_avail(void)                 /* version 2: none, core B writes the DAC */
{                                               /* ring itself (ETUDE §103) */
    return 0;
}

/* One round: DMA position, core B's PCM into the DAC ring (SYSRAM), input, doorbell when core B
 * may have something to do (new input, or 4 KiB of its output ring freed) */
static int dac_switch;                          /* the next track's rate waits for the drain */

/* The state of the track open (non-blocking playback, below) */
static struct {
    int flags, warm, kept, changed, full, open, opening;    /* opening: async, headers read */
    uint32_t t0, t_load, t_ready, t_open, t_dac, t_first, t_play, ur0, lost0, i2s0, fb, t_req;
    uint32_t na_frames, na_t, na_in;    /* the last change of the frames decoded: when, input */
    uint32_t st_frames, st_t;           /* the same for the stall watch (0.99r34) */
    int32_t fill0;
    uint32_t in_min, out_min, out_max, in_empty;  /* perf: the rings once the DAC ring is full */
    uint32_t laps0;                     /* audio.laps_lost at the open */
    int in_was_empty, in_full;  /* the input ring filled once: its minimum counts from then */
} ps;

static int perf_load(uint32_t img)
{
    const char *pp = perf_begin("core B load");
    int r = hifi_load_own(img);
    perf_end(pp);
    return r;
}

static void perf_rings(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    uint32_t out = out_avail(), in = c->in_wr - c->rb_rd;
    if (out < ps.out_min)
        ps.out_min = out;
    if (out > ps.out_max)
        ps.out_max = out;
    if (c->in_end != 0xffffffffu && feed_pos >= c->in_end)
        return;                                 /* all fed: the input may run out */
    if (!ps.in_full && in < HP_IN_SIZE - 8192)
        return;                                 /* a track starting: its ring not filled yet */
    ps.in_full = 1;
    if (in < ps.in_min)
        ps.in_min = in;
    if (!in && !out && !ps.in_was_empty)
        ps.in_empty++;                          /* core B waits for the card: an episode */
    ps.in_was_empty = !in && !out;
}

static void own_service(void)
{
    perf_tick(&perf_play);
    audio_update();
    if (ps.full && !dac_switch)
        perf_rings();
    feed();
    if (fed_since_ring) {
        fed_since_ring = freed_since_ring = 0;
        hifi_ring();
    }
}

/* Where core B's first frames go after a start or a rate change: 40 ms of silence ahead of the
 * DMA (8 KiB at least), the time core B takes to decode them */
static uint32_t pcm_lead(void)
{
    uint32_t lead = audio.rate * audio.frame_bytes / 25;
    lead = lead < 8192 ? 8192 : lead;
    return (audio.rd + lead) & ~7u;
}

/* Core B writes the DAC ring from 'wr' on (ETUDE §103): the DMA's position and time as core A
 * last read them, its rate, core B's clock; then core A keeps off the write position until
 * player_close() */
static void pcm_release(uint32_t wr)
{
    volatile struct hp_ctl *c = HP_CTL;
    audio_update();
    if ((int32_t)(wr - audio.rd) < 1024) {      /* the DMA past it (a skip, a seek: the open */
        wr = pcm_lead();                        /* took longer than the silence ahead): the */
        audio_zero(audio.rd, audio.ring_size);  /* whole ring silent, a late start plays that */
    }
    audio.wr = wr;
    c->pcm_wr = wr;
    c->pcm_rd = audio.rd;
    c->pcm_pos = audio.last_pos;
    c->pcm_t = timer24();
    c->pcm_bps = audio_bps();
    c->pcm_min = 0xffffffffu;
    c->b_hz = hifi_hz;
    audio_attach(&c->pcm_wr);
    c->pcm_hold = 0;
    hifi_ring();
}

/* Read buffer of the three Ogg scans below (they run one after the other): 8 KiB, and a page
 * header across the end of the previous read */
static uint8_t ogg_buf[8192 + 27];

/* Ogg: granule position of the last page (the stream length), from the end of the file;
 * 0 when none is found in the last 64 KiB */
static uint64_t ogg_end_granule(const struct fat_file *f)
{
    uint8_t *buf = ogg_buf;
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
            uint32_t lo = le32(h + 6), hi = le32(h + 10);
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
    uint8_t *b = ogg_buf;
    struct fat_cursor c = { 0, 0 };
    uint32_t n = f->size < 512 ? f->size : 512;
    if (fat_pread(f, &c, 0, b, n) != (int)n || n < 27 || b[0] != 'O' || b[3] != 'S' ||
        27u + b[26] + 16 > n)
        return 0;
    const uint8_t *p = b + 27 + b[26];
    if (!__builtin_memcmp(p, "OpusHead", 8))
        return 48000;
    if (p[0] == 1 && !__builtin_memcmp(p + 1, "vorbis", 6))
        return le32(p + 12);
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
    uint8_t *b = ogg_buf;
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
                uint32_t lo32 = le32(h + 6), hi32 = le32(h + 10);
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
static const uint32_t zero[256];                /* 1 KiB of silence, behind a track's end */

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
 * level would do. The top level from the start of a track (load, open) until the DAC ring is full
 * (0.36 went up on every peak: Opus switched 29 times in 2 min between 133 and 200 MHz,
 * ETUDE §24). */
static const uint8_t gov_div[] = { 8, 7, 6, 5, 4, 3, 2 };   /* 50 57 67 80 100 133 200 MHz */
#define GOV_LEVELS ((int)sizeof gov_div)
static int player_clock;                               /* 0: automatic, else a fixed divider */
static struct {
    int level, boost;
    uint32_t t0, t_last, idle_last, low_ms;
    int32_t fill_last;
    uint32_t windows, sum_mhz10, max_mhz10, max_load, changes, ms[GOV_LEVELS];
    uint32_t sum_ms, ups, downs, safety, min_fill_ms;
    uint32_t acc_busy, acc_el;                  /* the load judged over 250 ms at least */
    uint64_t wsum_mhz10;                        /* MHz used x ms: the mean over time */
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
    gov.min_fill_ms = 0xffffffffu;
    gov.t0 = gov.t_last = timer24();
}

/* Once core B runs our runtime: the window starts here */
static void gov_begin(void)
{
    gov.t_last = timer24();
    gov.idle_last = b_idle();
    gov.fill_last = audio_fill();
}

/* The window (0.99r29, ETUDE §107): a quarter of the PCM ring's duration at the rate playing,
 * 16 to 250 ms (250 at 44.1 kHz / 16 bits, 16 at 192 kHz / 24 bits: the ring lasts 64 ms) */
static uint32_t gov_window_ticks(void)
{
    uint32_t bps = audio.rate * audio.frame_bytes;
    uint32_t ms = bps ? (uint32_t)((uint64_t)audio.ring_size * 1000 / bps) / 4 : 250;
    ms = ms < 16 ? 16 : ms > 250 ? 250 : ms;
    return ms * 24000;                          /* TIMER1: 24 MHz */
}

static void gov_window(void)
{
    uint32_t now = timer24(), el = now - gov.t_last;
    if (el < gov_window_ticks())
        return;
    uint32_t idle = b_idle(), di = idle - gov.idle_last;
    uint32_t busy = el > di ? el - di : 0;
    uint32_t load = (uint32_t)((uint64_t)busy * 1000 / el);     /* per mille */
    uint32_t mhz10 = hifi_hz / 100000 * load / 1000;            /* tenths of MHz used */
    gov.windows++;
    gov.sum_mhz10 += mhz10;
    gov.sum_ms += el / 24000;
    gov.wsum_mhz10 += (uint64_t)mhz10 * (el / 24000);
    player.b_load = load;
    player.b_mhz10 = mhz10;
    if (mhz10 > gov.max_mhz10)
        gov.max_mhz10 = mhz10;
    if (load > gov.max_load)
        gov.max_load = load;
    if (gov.level >= 0)
        gov.ms[gov.level] += el / 24000;
    int32_t fill = audio_fill();
    uint32_t bps = audio.rate * audio.frame_bytes;
    int32_t ring_ms = bps ? (int32_t)((uint64_t)audio.ring_size * 1000 / bps) : 0;
    int32_t fill_ms = bps && fill > 0 ? (int32_t)((uint64_t)fill * 1000 / bps) : 0;
    if (!gov.boost && (uint32_t)fill_ms < gov.min_fill_ms)
        gov.min_fill_ms = (uint32_t)fill_ms;
    if (!player_clock) {
        if (gov.boost) {
            if (fill >= (int32_t)audio.ring_size - 16384 || now - gov.t0 > 72000000)
                gov.boost = 0;                  /* DAC ring full (or 3 s): end of the boost */
        } else {
            /* the lowest level that keeps the load at 75 % (ETUDE §107) */
            uint32_t need10 = mhz10 * 4 / 3;
            int want = GOV_LEVELS - 1;
            for (int i = 0; i < GOV_LEVELS; i++)
                if (4000 / gov_div[i] >= need10) {
                    want = i;
                    break;
                }
            /* the load over 250 ms at least (a short window sees a codec's bursts) */
            gov.acc_busy += busy;
            gov.acc_el += el;
            int judged = gov.acc_el >= 6000000;
            uint32_t lload = judged ? (uint32_t)((uint64_t)gov.acc_busy * 1000 / gov.acc_el) : 0;
            if (judged) {
                uint32_t lmhz10 = hifi_hz / 100000 * lload / 1000;
                need10 = lmhz10 * 4 / 3;
                want = GOV_LEVELS - 1;
                for (int i = 0; i < GOV_LEVELS; i++)
                    if (4000 / gov_div[i] >= need10) {
                        want = i;
                        break;
                    }
                gov.acc_busy = gov.acc_el = 0;
            }
            if (ring_ms && fill_ms < ring_ms / 4) {         /* the ring under a quarter */
                if (gov.level < GOV_LEVELS - 1)
                    gov.safety++;
                gov_set(GOV_LEVELS - 1);
                gov.low_ms = 0;
            } else if (ring_ms && fill_ms < ring_ms / 2) {  /* under half: a level up at least */
                if (gov.level < GOV_LEVELS - 1) {
                    gov.safety++;
                    gov_set(want > gov.level ? want : gov.level + 1);
                }
                gov.low_ms = 0;
            } else if (!judged) {
                ;                                           /* the load not judged yet */
            } else if (lload > 850 && gov.level < GOV_LEVELS - 1) { /* over 85 %: up at once */
                gov_set(want > gov.level ? want : gov.level + 1);
                gov.ups++;
                gov.low_ms = 0;
            } else if (lload < 650 && want < gov.level) {   /* under 65 % for 1 s: straight */
                gov.low_ms += 250;                          /* down to the level for 75 % */
                if (gov.low_ms >= 1000) {
                    gov_set(want);
                    gov.downs++;
                    gov.low_ms = 0;
                }
            } else
                gov.low_ms = 0;
        }
    }
    gov.fill_last = fill;
    gov.t_last = now;
    gov.idle_last = idle;
}

static void gov_report(const char *name)
{
    uint32_t mean = gov.sum_ms ? (uint32_t)(gov.wsum_mhz10 / gov.sum_ms) : 0;
    log_printf("clock: %s: core B %s, used mean %u.%u MHz, max %u.%u MHz (window), max load "
               "%u.%u %%; ms at 50/57/67/80/100/133/200 MHz: %u %u %u %u %u %u %u; %u changes "
               "(%u up, %u down, %u for the ring); lowest fill %u ms", name,
               player_clock ? "fixed" : "automatic", mean / 10, mean % 10, gov.max_mhz10 / 10,
               gov.max_mhz10 % 10, gov.max_load / 10, gov.max_load % 10, gov.ms[0], gov.ms[1],
               gov.ms[2], gov.ms[3], gov.ms[4], gov.ms[5], gov.ms[6], gov.changes, gov.ups,
               gov.downs, gov.safety, gov.min_fill_ms == 0xffffffffu ? 0 : gov.min_fill_ms);
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

static int open_step(void);
static int open_finish(void);

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
    if (!ps.kept) {                             /* nothing heard before it (a start, a jump): */
        perf_play.last = 0;                     /* no gap of the audio path to count until its */
        perf_play.max = 0;                      /* first service (r9: 8 s of browsing counted) */
    }
    drain.on = 0;
    uint32_t sh = audio_rebase();               /* core B stopped: the counters kept small */
    tail.end -= sh;
    ps.fill0 = ps.kept ? audio_fill() : 0;      /* what the DAC ring holds at the switch */
    ps.ur0 = audio.ur_all;                      /* the counters of this track by difference */
    ps.lost0 = audio.lost_all;
    ps.i2s0 = audio.i2s_all;
    ps.in_min = ps.out_min = 0xffffffffu;
    ps.out_max = ps.in_empty = 0;
    ps.laps0 = audio.laps_lost;
    ps.in_was_empty = 0;                        /* the gaps run on: the one before this track
                                                 * (its tags read, core B loaded) counts in it */
    log_printf("player: %s, %u bytes, our codec %u", f->name, f->size, codec);
    mp4_on = codec == HP_CODEC_AAC || codec == HP_CODEC_ALAC;
    if (mp4_on) {                               /* the container read here, the seek done here */
        struct mp4_track t;
        r = mp4_open(file, seek, &t);         /* own_file: f may be the caller's copy */
        if (r) {
            player.why = r == MP4_EUNSUPPORTED ? t.why : HP_WHY_NONE;
            return fail(r == MP4_EREAD ? HIFI_EREAD : r == MP4_EFORMAT ? PLAYER_EFORMAT :
                        PLAYER_EUNSUPPORTED, r == MP4_EUNSUPPORTED ?
                        "MP4: neither AAC-LC nor ALAC as we play them" :
                        "MP4: container not readable");
        }
        seek = 0;
        codec = t.codec;                        /* an M4A: AAC or ALAC, the container says */
    }
    player.codec = codec;
    gov_start();                                /* automatic: top level for the load and open */
    uint32_t img = codec == HP_CODEC_AIFF ? HP_CODEC_WAV : codec;  /* one image for PCM */
    if (hifi_own_loaded() == img && !hifi_restart_own())
        ps.warm = 1;                            /* the image is still in the HIGHRAM */
    else if ((r = perf_load(img)))
        return fail(r, "our codec is not in the firmware image");
    ps.t_load = ticks();
    if ((r = hifi_start_own()))
        return fail(r, "our core B runtime did not start");
    c->pcm_hold = 1;                            /* until the DACs are ready for this track */
    ps.t_ready = ticks();
    gov_begin();
    if (ps.kept)
        audio_update();
    c->in_end = mp4_on ? 0xffffffffu : f->size; /* known from the start (MP3 length estimate) */
    feed_start();                               /* the stream from its start (version 3) */
    c->trk.codec = codec;
    c->trk.size = c->in_end;
    c->trk.comment_end_page = codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS ? cpage : 0;
    c->trk.comment_end_seg = cseg;
    c->trk.seek_ms = seek;
    c->trk.seek_offset = 0;
    if (c->trk.comment_end_page)
        log_printf("player: comment header ends at page %u, segment %u", c->trk.comment_end_page,
                   c->trk.comment_end_seg);
    uint64_t end_g = 0;
    if (codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS) {
        end_g = ogg_end_granule(file);
        c->trk.end_granule_lo = (uint32_t)end_g;
        c->trk.end_granule_hi = (uint32_t)(end_g >> 32);
        cur.index = cur.cluster = 0;
        log_printf("player: last Ogg granule %u", (uint32_t)end_g);
    }
    if (seek && (codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS)) {
        /* the page before the target: playback starts up to a page early, nothing decoded for
         * nothing (§75) */
        uint32_t rate = ogg_rate(f), reads = 0, t_seek = ticks();
        uint64_t g = (uint64_t)seek * rate / 1000;
        if (rate && g)
            c->trk.seek_offset = ogg_page_before(f, g, end_g, cpage, &reads);
        cur.index = cur.cluster = 0;
        log_printf("player: start at %u ms: Ogg page at %u, %u reads in %u us", seek,
                   c->trk.seek_offset, reads, ticks() - t_seek);
    } else if (seek)
        log_printf("player: start at %u ms", seek);
    feed();
    command(HP_CMD_OPEN);
    player.state = ST_OPEN;
    ps.t_req = ticks();
    if (flags & PLAY_ASYNC) {                   /* the headers read while the loop goes on: */
        ps.open = ps.opening = 1;               /* player_service() finishes it (0.99r25) */
        return 0;
    }
    for (;;) {
        wdt_kick();
        if (ps.kept)
            audio_update();
        int w = open_step();
        if (w < 0)
            return w;
        if (w)
            break;
    }
    return open_finish();
}

/* One round of the open: the input fed; 1 once core B knows the format, < 0 on a failure */
static int open_step(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    feed();
    if (fed_since_ring) {
        fed_since_ring = 0;
        hifi_ring();
    }
    if (c->state == HP_FORMAT)
        return 1;
    if (c->state == HP_ERROR || ticks() - ps.t_req > 3000000) {
        log_printf("player: open failed: state %u error %u why %u, fault pc %x lr %x cfsr %x, "
                   "in %u/%u", c->state, c->error, c->why, c->fault_pc, c->fault_lr,
                   c->fault_cfsr, c->in_rd, feed_pos);
        int r = c->state == HP_ERROR ? b_error(c) : HIFI_ETIMEOUT;
        hifi_stop();
        audio_detach();
        ps.open = ps.opening = 0;
        return fail(r, "our codec could not open the file");
    }
    return 0;
}

/* The format known: the DACs, core B released on the ring */
static int open_finish(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    const struct fat_file *f = &own_file;
    ps.opening = 0;
    ps.t_open = ticks();
    player.rate = c->rate;
    player.channels = c->channels;
    player.bps = c->bits;
    player.length_ms = player.rate ? (uint32_t)((uint64_t)c->total_frames * 1000 / player.rate) : 0;
    log_printf("player: format: %u Hz, %u ch, %u bits, %u-byte frames, %u ms, after %u bytes of "
               "input%s", player.rate, player.channels, player.bps, c->frame_bytes,
               player.length_ms, c->in_rd, c->codec == HP_CODEC_VORBIS && (c->error_info >> 31) ?
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
        audio_ring_direct(1);
        audio_start(player.rate, ps.fb == 4);
        audio_update();
        pcm_release(pcm_lead());                /* the zeroed ring, then core B's PCM */
    } else if (ps.kept && !dac_switch)
        pcm_release(audio.wr);                  /* right after the last track's frames */
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
#define STALL_US      500000u                   /* core B stuck (0.99r34): the DMA replays the
                                                 * ring meanwhile */

/* A fault or a stall of core B (rework step 7): the fault record, and its codec forgotten so that
 * the next open loads it again (its code may be damaged) */
static void b_crash(const char *what, volatile struct hp_ctl *c)
{
    struct crash_info ci = { .core = CRASH_B, .what = what, .pc = c->fault_pc,
                             .lr = c->fault_lr, .cfsr = c->fault_cfsr };
    crash_record(&ci);
    hifi_own_drop();
}
#define NOAUDIO_BYTES (1u << 20)

/* One round of an open track: 0 while it plays, 1 at its end (all of core B's output in the DAC
 * ring with PLAY_CHAIN, the DAC ring drained otherwise), < 0 on an error */
int player_service(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    if (!ps.open)
        return player.state == ST_ERROR ? player.error : 1;
    wdt_kick();
    if (ps.opening) {                           /* an open of PLAY_ASYNC: its headers */
        perf_tick(&perf_play);
        audio_update();
        gov_window();
        int w = open_step();
        if (w <= 0)
            return w;
        const char *pp = perf_begin("open");
        w = open_finish();
        perf_end(pp);
        if (w)
            return w;
    }
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
            audio_zero(audio.rd, audio.ring_size);  /* only its start was cleared */
            pcm_release(pcm_lead());
            ps.changed = 1;
        } else {
            audio_ring_direct(1);
            audio_start(player.rate, ps.fb == 4);
            audio_update();
            pcm_release(pcm_lead());
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
        c->pcm_min = 0xffffffffu;               /* core B's too (a statistic only) */
    }
    /* frames heard: written by core B minus what is still in the two rings */
    uint32_t queued = (out_avail() + (uint32_t)(audio_fill() > 0 ? audio_fill() : 0)) / ps.fb;
    player.frames = c->start_frame + (c->frames > queued ? c->frames - queued : 0);
    player.time_ms = player.rate ? (uint32_t)((uint64_t)player.frames * 1000 / player.rate) : 0;
    heard_update();
    if (c->state == HP_ERROR) {
        log_printf("player: core B error %u, fault pc %x lr %x cfsr %x, at %u MHz", c->error,
                   c->fault_pc, c->fault_lr, c->fault_cfsr, hifi_hz / 1000000);
        if (c->error == HP_ERR_FAULT)
            b_crash("core B fault", c);
        player.error = b_error(c);
        return player.error;
    }
    /* Core B stuck (0.99r34): no frame for STALL_US while the DMA plays, with room in the PCM
     * ring and input in the input ring (not in a pause, a rate change, an open) */
    if (c->frames != ps.st_frames || !ps.st_t || dac_switch || ps.opening || audio_paused ||
        !audio.on || c->state != HP_PLAYING || c->pcm_hold ||
        audio_fill() + 8192 > (int32_t)HP_OUT_SIZE || c->in_wr - c->rb_rd < 4096) {
        ps.st_frames = c->frames;
        ps.st_t = ticks() | 1;
    } else if (ticks() - ps.st_t > STALL_US) {
        log_printf("player: core B stuck: no frame for %u ms (room %u bytes, input %u bytes)",
                   (ticks() - ps.st_t) / 1000, HP_OUT_SIZE - (uint32_t)audio_fill(),
                   c->in_wr - c->rb_rd);
        b_crash("core B stall", c);
        player.why = HP_WHY_NONE;
        player.error = PLAYER_EFAULT;
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

/* perf (ETUDE §97): the gaps of this track and the rings' extremes once the DAC ring was full;
 * in ms of sound, the input at the file's mean rate */
static uint32_t ms_of(uint32_t bytes, uint32_t per_s)
{
    return per_s ? (uint32_t)((uint64_t)bytes * 1000 / per_s) : 0;
}

static void perf_report(const char *name)
{
    uint32_t pcm_s = audio.rate * audio.frame_bytes;   /* the DACs' rate (a low rate is raised) */
    uint32_t in_s = player.length_ms ? (uint32_t)((uint64_t)own_file.size * 1000 /
                                                  player.length_ms) : 0;
    const struct perf_gap *a = &perf_play, *l = &perf_loop;
    static const char *const cn[] = { "?", "WAV", "FLAC", "MP3", "Vorbis", "Opus", "AIFF", "AAC",
                                      "ALAC", "MPC" };
    const char *codec = player.codec < sizeof cn / sizeof *cn ? cn[player.codec] : "?";
    if (!ps.full) {
        log_printf("perf: %s [%s %u/%u]: gap max %u ms (%s %u ms), loop gap max %u ms (%s %u "
                   "ms); DAC ring never full", name, codec, player.rate, player.bps, a->max / 1000,
                   a->max_ph ? a->max_ph : "-", a->max_ph_us / 1000, l->max / 1000,
                   l->max_ph ? l->max_ph : "-", l->max_ph_us / 1000);
        return;
    }
    int32_t dmin = audio.min_fill > 0 ? audio.min_fill : 0;
    log_printf("perf: %s [%s %u/%u]: gap max %u ms (%s %u ms), loop gap max %u ms (%s %u ms); "
               "DAC min %u ms; in min %u B (%u ms); out min %u B max %u B; in empty %u; laps "
               "lost %u; core B underruns %u (%u B), min %u ms",
               name, codec,
               player.rate, player.bps, a->max / 1000,
               a->max_ph ? a->max_ph : "-", a->max_ph_us / 1000, l->max / 1000,
               l->max_ph ? l->max_ph : "-", l->max_ph_us / 1000,
               ms_of((uint32_t)dmin, pcm_s),
               ps.in_min == 0xffffffffu ? 0 : ps.in_min,
               ps.in_min == 0xffffffffu ? 0 : ms_of(ps.in_min, in_s),
               ps.out_min == 0xffffffffu ? 0 : ps.out_min, ps.out_max, ps.in_empty,
               audio.laps_lost - ps.laps0, HP_CTL->pcm_ur, HP_CTL->pcm_lost,
               HP_CTL->pcm_min == 0xffffffffu ? 0 : ms_of(HP_CTL->pcm_min, pcm_s));
}

/* The end of an open track: 'stopped' by the user, or at its end (or after an error) */
void player_close(int stopped)
{
    volatile struct hp_ctl *c = HP_CTL;
    read_errors_said();
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
    perf_report(own_file.name);
    perf_restart(&perf_play);                   /* the next gap still counts from here */
    perf_restart(&perf_loop);
    gov_report(own_file.name);
    if (c->synth_gran || c->synth_soft)
        log_printf("synth: %s: %u granules on SYNTH, %u timeouts (software for the rest)",
                   own_file.name, c->synth_gran, c->synth_soft);
#if !defined(ECHO_NO_HWFFT) || defined(ECHO_HWTEST)
    if (c->fft_blocks || c->fft_soft)
        log_printf("fft: %s: %u IMDCT blocks on HIFIACC, %u in software after a timeout",
                   own_file.name, c->fft_blocks, c->fft_soft);
#endif
#ifdef ECHO_FFTCMP
    if (c->hwf.magic == HWT_MAGIC && c->hwf.blocks)  /* test build: sound from HIFIACC */
        log_printf("fcmp: %s: %u blocks (%u short), %u samples: %u exact, %u within 1, max %u "
                   "LSB at block %u (index %u): e_hw %u e_sw %u, %u-point, |hw| OR %x",
                   own_file.name, c->hwf.blocks, c->hwf.short_blocks, c->hwf.samples,
                   c->hwf.exact, c->hwf.within1, c->hwf.max, c->hwf.worst_at, c->hwf.worst_i,
                   c->hwf.worst_info & 0xff, c->hwf.worst_info >> 8 & 0xff,
                   1u << (c->hwf.worst_info >> 16), c->hwf.worst_m);
#endif
#ifdef ECHO_HWTEST
    if (c->hwf.magic == HWT_MAGIC && c->hwf.blocks)  /* test build: FFT against the software */
        log_printf("hwf: %s: %u IMDCT blocks (%u short), %u samples: %u exact, %u within 1, "
                   "max %u LSB; hw %u us, software %u us in all; %u timeouts", own_file.name,
                   c->hwf.blocks, c->hwf.short_blocks, c->hwf.samples, c->hwf.exact,
                   c->hwf.within1, c->hwf.max, c->hwf.hw_ticks / 24, c->hwf.sw_ticks / 24,
                   c->hwf.timeouts);
#endif
    if (c->alac_hw || c->alac_low || c->alac_quant || c->alac_long || c->alac_soft)
        log_printf("alacacc: %s: %u predictions on HIFIACC; in software: %u of order 1 or 2, "
                   "%u lpc_quant >= 12, %u over 8191 samples, %u after a timeout", own_file.name,
                   c->alac_hw, c->alac_low, c->alac_quant, c->alac_long, c->alac_soft);
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
    if (c->hws.magic == HWT_MAGIC && c->hws.p_cmp && !c->hws.slots)  /* SYNTH against libmpcdec */
        log_printf("hwm: %s: %u frames, %u samples: %u exact, %u within 1, max %u; SYNTH %u us, "
                   "libmpcdec %u us; %u timeouts", own_file.name, c->hws.p_gran, c->hws.p_cmp,
                   c->hws.p_exact, c->hws.p_within1, c->hws.p_max, c->hws.p_hw_ticks / 24,
                   c->hws.syn_sw_ticks / 24, c->hws.p_timeouts);
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
            log_printf("hws: synth time: hw %u us, sw %u us over %u calls (18 slots each); "
                       "IRQ 1 pending at %u of %u ends", c->hws.syn_hw_ticks / 24,
                       c->hws.syn_sw_ticks / 24, c->hws.syn_timed, c->hws.syn_irq, c->hws.polled);
        if (c->hws.im_calls) {
            if (c->hws.im_polled)
                log_printf("hwi: IRQ 7 pending after %u..%u ticks (24 MHz), mean %u",
                           c->hws.im_lat_min, c->hws.im_lat_max,
                           c->hws.im_lat_sum / c->hws.im_polled);
            log_printf("hwi: mode %u shift %d: %u transforms (%u IRQ 7, %u timeouts); hw %u us, "
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
    if (c->hwa.magic == HWT_MAGIC && (c->hwa.subframes || c->hwa.skipped)) {
        /* test build: HIFIACC's ALAC filter against our predictor (ETUDE §113) */
        log_printf("hwa: %s: %u predictions, coefficients reversed: %u exact, %u differ; in stream "
                   "order: %u exact, %u differ; %u skipped, %u timeouts; hw %u us (DMA "
                   "included), software %u us", own_file.name, c->hwa.subframes, c->hwa.exact,
                   c->hwa.differ, c->hwa.s_exact, c->hwa.s_differ, c->hwa.skipped,
                   c->hwa.timeouts, c->hwa.hw_ticks / 24, c->hwa.sw_ticks / 24);
        char b[240], *p = b, *end = b + sizeof b - 1;
        p = log_format(p, end, "hwa: per order (exact/differ):");
        for (int k = 1; k <= 30; k++)
            if (c->hwa.ok_o[k] || c->hwa.diff_o[k])
                p = log_format(p, end, " %d:%u/%u", k, c->hwa.ok_o[k], c->hwa.diff_o[k]);
        p = log_format(p, end, "; per lpc_quant:");
        for (int k = 0; k < 16; k++)
            if (c->hwa.ok_q[k] || c->hwa.diff_q[k])
                p = log_format(p, end, " %d:%u/%u", k, c->hwa.ok_q[k], c->hwa.diff_q[k]);
        *p = 0;
        log_printf("%s", b);
        if (c->hwa.differ)
            log_printf("hwa: first difference: order %u, lpc_quant %u, %u bits, %u samples, at "
                       "%u: ours %d, hw %d", c->hwa.f_order, c->hwa.f_quant, c->hwa.f_bps,
                       c->hwa.f_n, c->hwa.f_idx, c->hwa.f_sw, c->hwa.f_hw);
        c->hwa.magic = 0;
    }
#endif
    command(HP_CMD_STOP);
    hifi_stop();
    audio_detach();                             /* the write position is core A's again */
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

int player_settled(void)
{
    return ps.open && ps.full;
}

int player_is_open(void)
{
    return ps.open;
}

const char *player_file_name(void)
{
    return own_file.name;
}
