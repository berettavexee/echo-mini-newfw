/* Step 5c player: a stock codec module on core B, driven through the mailbox as the stock
 * firmware does (protocol of ETUDE §1.2, checked in emulation by tools/emu_host.py):
 *  - A2B ch2 0x65: file descriptor {u8 handle, h2, h3, pad | size | pos | pos2 | pos3 | codec}
 *    in core A RAM; B2A ch2 0x66 when taken;
 *  - A2B ch1: 1 OPEN, 4 DECODE, 0xd CLOSE; B2A ch0: 3 opened, 5 decoded (data = &info), 2 open
 *    failed, 6 end or error, 0xe closed;
 *  - B2A ch2 file requests, served at once: 0x69 seek {off, whence, h} (new position written
 *    back at +0, reply 0x6a 0), 0x6b read {buf, size, h} (data written into core B RAM, new
 *    position at +4, reply 0x6c count), 0x73 close (reply 0x74 1).
 * Info: +0x00 time ms, +0x04 length ms, +0x0c bits, +0x10 rate, +0x14 channels, +0x1c PCM
 * (16-bit stereo interleaved), +0x20 frames. The PCM goes into the ring as packed 16-bit
 * frames (the same bytes), and DECODE is only sent once the buffer is in the ring. */
#include "player.h"
#include "audio.h"
#include "board.h"
#include "hifi.h"
#include "log.h"

enum { ST_IDLE, ST_LOAD, ST_OPEN, ST_PLAY, ST_DRAIN, ST_END, ST_ERROR };

struct player_state player;

static struct {
    uint8_t h[4];
    uint32_t size, pos, pos2, pos3, codec;
} desc __attribute__((aligned(4)));

static const struct fat_file *file;
static struct fat_cursor cur;
static uint32_t pos[8];

static uint32_t rd32(uint32_t a)
{
    return *(volatile uint32_t *)hifi_ptr(a);
}

static void wr32(uint32_t a, uint32_t v)
{
    *(volatile uint32_t *)hifi_ptr(a) = v;
}

static int tracing;                             /* start-up phase: every message logged */
static uint32_t t_trace;
#define MIN_REPLY_US 200                        /* file replies never sooner than this */

/* Answers one file request; returns 0 if 'cmd' was a file request */
static int serve(uint32_t cmd, uint32_t data)
{
    uint32_t t_req = ticks();
    if (cmd == 0x6b) {                          /* read {buf, size, h} */
        uint32_t buf = rd32(data), n = rd32(data + 4), h = rd32(data + 8) & 7;
        uint32_t p = pos[h], left = p < file->size ? file->size - p : 0;
        if (n > left)
            n = left;
        if (n && fat_pread(file, &cur, p, hifi_ptr(buf), n) != (int)n) {
            log_printf("player: read error at %u (+%u)", p, n);
            n = 0;
        }
        pos[h] = p + n;
        wr32(data + 4, pos[h]);
        player.reads++;
        player.read_bytes += n;
        while (ticks() - t_req < MIN_REPLY_US)
            ;
        if (tracing)
            log_printf("player: +%u us read h%u %u at %u into %x, reply after %u us",
                       t_req - t_trace, h, n, p, buf, ticks() - t_req);
        hifi_send(2, 0x6c, n);
        return 0;
    }
    if (cmd == 0x69) {                          /* seek {off, whence, h} */
        int32_t off = (int32_t)rd32(data);
        uint32_t wh = rd32(data + 4), h = rd32(data + 8) & 7;
        int64_t p = wh == 0 ? off : wh == 1 ? (int64_t)pos[h] + off : (int64_t)file->size + off;
        int ok = p >= 0 && p <= file->size;
        if (ok) {
            pos[h] = (uint32_t)p;
            wr32(data, pos[h]);
        }
        player.seeks++;
        while (ticks() - t_req < MIN_REPLY_US)
            ;
        if (tracing)
            log_printf("player: +%u us seek h%u %d/%u -> %u", t_req - t_trace, h, off, wh, pos[h]);
        hifi_send(2, 0x6a, ok ? 0 : 0xffffffffu);
        return 0;
    }
    if (cmd == 0x73) {                          /* close */
        while (ticks() - t_req < MIN_REPLY_US)
            ;
        hifi_send(2, 0x74, 1);
        return 0;
    }
    return -1;
}

/* Serves file requests until a control message among 'want' (bit mask of commands < 32)
 * arrives; 0 on success */
static int wait_for(uint32_t want, uint32_t *cmd, uint32_t *data, uint32_t us)
{
    int ch;
    for (uint32_t t0 = ticks(); ticks() - t0 < us;) {
        wdt_kick();
        audio_update();
        if (!hifi_poll(&ch, cmd, data))
            continue;
        if (tracing && !(ch == 2 && (*cmd == 0x6b || *cmd == 0x69)))
            log_printf("player: +%u us B2A ch%d %x data %x", ticks() - t_trace, ch, *cmd, *data);
        if (ch == 2) {
            if (serve(*cmd, *data) && *cmd != 0x66 && *cmd != 0x68 && *cmd != 0x75 && *cmd != 0xd3)
                log_printf("player: unknown file request %x data %x", *cmd, *data);
            if (*cmd == 0x66 && (want & 1u << 31))
                return 0;
            continue;
        }
        if (*cmd < 32 && (want & 1u << *cmd))
            return 0;
        if (*cmd != 5)                          /* a buffer decoded before a CLOSE: expected */
            log_printf("player: unexpected message ch%d %x data %x", ch, *cmd, *data);
    }
    return HIFI_ETIMEOUT;
}

static int fail(int err, const char *what)
{
    tracing = 0;
    player.error = err;
    player.state = ST_ERROR;
    log_printf("player: %s (%d)", what, err);
    return err;
}

int player_play(const struct fat_file *f, int module, int codec, int (*ui)(void))
{
    hifi_set_div(4);                            /* stock modules: 100 MHz, as validated */
    struct fat_file img;
    uint32_t cmd, data, info = 0;
    int r, ch_end;

    for (unsigned i = 0; i < sizeof player / sizeof(uint32_t); i++)
        ((uint32_t *)&player)[i] = 0;
    file = f;
    cur.index = cur.cluster = 0;
    for (int i = 0; i < 8; i++)
        pos[i] = 0;
    player.state = ST_LOAD;
    log_printf("player: %s, %u bytes, module %d", f->name, f->size, module);
    if ((r = hifi_find_image(module, &img)))
        return fail(r, "no stock image with this module in the root");
    for (int attempt = 1;; attempt++) {
        if ((r = hifi_load(&img, module)))
            return fail(r, "module load failed");
        tracing = 1;
        t_trace = ticks();
        if ((r = hifi_start()))
            return fail(r, "core B did not start");
        for (int i = 0; i < 8; i++)
            pos[i] = 0;
        cur.index = cur.cluster = 0;
        desc.h[0] = 3;
        desc.h[1] = 4;
        desc.h[2] = 5;
        desc.size = f->size;
        desc.pos = desc.pos2 = desc.pos3 = 0;
        desc.codec = (uint32_t)codec;
        log_printf("player: +%u us A2B ch2 65 (descriptor), attempt %d", ticks() - t_trace, attempt);
        hifi_send(2, 0x65, (uint32_t)&desc);
        if (!(r = wait_for(1u << 31, &cmd, &data, 500000)))
            break;
        hifi_dump("descriptor not taken");
        if (attempt == 3)
            return fail(r, "descriptor not taken");
        log_printf("player: restarting core B");
    }
    player.state = ST_OPEN;
    hifi_send(1, 1, 0);                         /* OPEN */
    if ((r = wait_for(1u << 2 | 1u << 3, &cmd, &data, 3000000))) {
        hifi_dump("no answer to OPEN");
        return fail(r, "no answer to OPEN");
    }
    if (cmd == 2)
        return fail(HIFI_EPROTO, "OPEN failed");
    info = data;
    player.length_ms = rd32(info + 4);
    player.bps = rd32(info + 0xc);
    player.rate = rd32(info + 0x10);
    player.channels = rd32(info + 0x14);
    tracing = 0;
    log_printf("player: opened: %u Hz, %u ch, %u bits, %u ms, info %x", player.rate,
               player.channels, player.bps, player.length_ms, info);
    if (player.bps != 16)
        return fail(HIFI_EPROTO, "only 16-bit output is handled");
    /* ring: memory layout B (HIGHRAM1 bank 3) when the module leaves it free (73 MP3 ends at
     * 0x01049730, 75 AAC at 0x01043658); the stock WAV module 76 uses HIGHRAM1 up to
     * 0x0105f2bc, so its ring goes to HIGHRAM0 bank 1, past its code (ETUDE §15.2, §17) */
    audio_start(player.rate, module == 76 ? RING_HIGHRAM0 : RING_HIGHRAM1, 1);
    ramp_hook = audio_update;                   /* volume ramps must not write into the ring */
    player.state = ST_PLAY;

    int have = 1, ended = 0, stop = 0;  /* message 3 carries a first buffer */
    uint32_t t_ui = 0, t_wait = 0;
    while (!stop) {
        wdt_kick();
        audio_update();
        int ch;
        if (hifi_poll(&ch, &cmd, &data)) {
            if (ch == 2) {
                if (serve(cmd, data))
                    log_printf("player: file request %x data %x ignored", cmd, data);
            } else if (cmd == 5) {
                info = data;
                have = 1;
            } else if (cmd == 6) {
                ended = 1;
                log_printf("player: end of stream (6) at %u ms, %u buffers", player.time_ms,
                           player.buffers);
            } else
                log_printf("player: unexpected message ch%d %x data %x", ch, cmd, data);
        }
        if (have) {
            uint32_t pcm = rd32(info + 0x1c), n = rd32(info + 0x20) * 4;
            if (n <= audio_free()) {
                if (t_wait && ticks() - t_wait > player.wait_us_max)
                    player.wait_us_max = ticks() - t_wait;
                audio_write(hifi_ptr(pcm), n);
                player.buffers++;
                player.frames += n / 4;
                player.time_ms = rd32(info);
                have = 0;
                t_wait = 0;
                if (!ended)
                    hifi_send(1, 4, 0);         /* DECODE the next buffer */
            } else if (!t_wait)
                t_wait = ticks();
        }
        if (ended && !have && audio_fill() <= 4096) {      /* drained (the last 12 ms play on) */
            player.state = ST_END;
            break;
        }
        if (ticks() - t_ui > 20000) {
            t_ui = ticks();
            if (ui())
                stop = 1;
        }
    }
    log_printf("player: %s after %u buffers, %u frames, %u ms; %u reads (%u bytes), %u seeks; "
               "ring underruns %u, i2s %u, min fill %u bytes, longest wait %u us",
               stop ? "stopped" : "finished", player.buffers, player.frames, player.time_ms,
               player.reads, player.read_bytes, player.seeks, audio.ring_ur, audio.i2s_ur,
               audio.min_fill, player.wait_us_max);
    ramp_hook = 0;
    hifi_send(1, 0xd, 0);                       /* CLOSE */
    if (wait_for(1u << 0xe, &cmd, &data, 1000000)) {
        log_printf("player: no answer to CLOSE");
        hifi_dump("no answer to CLOSE");
    }
    for (uint32_t t0 = ticks(); ticks() - t0 < 20000;)     /* late replies of the close */
        if (hifi_poll(&ch_end, &cmd, &data) && ch_end == 2)
            serve(cmd, data);
    if (logs.lost)
        log_printf("log: %u bytes lost so far (ring overflow)", logs.lost);
    hifi_dump("after CLOSE");
    hifi_stop();
    return stop ? 1 : 0;
}

/* ---- our runtime and codecs (ETUDE §19) ---- */
#include "hifi_proto.h"

static uint32_t fed_since_ring, freed_since_ring;

/* Input ring from the file, in pieces up to the ring end, as much as fits */
static void feed(void)
{
    volatile struct hp_ctl *c = HP_CTL;
    uint32_t wr = c->in_wr, rd = c->in_rd;
    if ((int32_t)(rd - wr) > 0) {               /* core B skipped past what we wrote */
        wr = rd;
        if (wr > file->size)
            wr = file->size;
        c->in_wr = wr;
        player.seeks++;
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

/* Ogg, for a seek: offset of the last page whose granule is <= target, by bisection on the
 * file (the first page with a granule after the middle of the range); 0 when none is found */
static uint32_t ogg_page_before(const struct fat_file *f, uint64_t target)
{
    static uint8_t b[4096 + 27];
    struct fat_cursor c = { 0, 0 };
    uint32_t lo = 0, hi = f->size, best = 0;
    for (int it = 0; it < 32 && hi > lo + 4096; it++) {
        uint32_t mid = lo + (hi - lo) / 2, at = mid, keep = 0;
        int found = 0;
        /* the first page after mid that ends a packet: pages are up to 64 KiB, read on */
        while (!found && at < f->size && at - mid < 72 * 1024) {
            uint32_t n = f->size - at < 4096 ? f->size - at : 4096;
            if (fat_pread(f, &c, at, b + keep, n) != (int)n)
                return best;
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
                if ((lo32 | (uint64_t)hi32 << 32) <= target) {
                    best = page;
                    lo = page + 27;
                } else
                    hi = mid;
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
        while (audio_free() < sizeof zero && ticks() - t0 < 2000000) {
            wdt_kick();
            audio_update();
        }
        audio_write(zero, sizeof zero);         /* 1 KiB of silence: the margin of the cut */
        while ((int32_t)(audio.rd - end) < 0 && ticks() - t0 < 2000000) {
            wdt_kick();                         /* until the DMA is past the last frame */
            audio_update();
        }
    }
    chain_done();
}

/* One round of a drain followed from the main loop: silence written behind the last frame as
 * the DMA frees room (the loop may come back late: the DMA never reaches stale data), 1 once
 * the DMA is past the last frame (or after 2 s of playing, a pause not counted) */
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
    return (int32_t)(audio.rd - drain.end) >= 0 || ticks() - drain.t0 >= 2000000;
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
    uint32_t t0, t_load, t_ready, t_open, t_dac, t_first, t_play, ur0, lost0, fb;
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
    ps.ur0 = audio.ring_ur;
    ps.lost0 = audio.ring_lost;
    log_printf("player: %s, %u bytes, our codec %u", f->name, f->size, codec);
    gov_start();                                /* automatic: top level for the load and open */
    if (hifi_own_loaded() == codec && !hifi_restart_own())
        ps.warm = 1;                            /* the image is still in the HIGHRAM */
    else if ((r = hifi_load_own(codec)))
        return fail(r, "our codec is not in the firmware image");
    ps.t_load = ticks();
    if ((r = hifi_start_own()))
        return fail(r, "our core B runtime did not start");
    ps.t_ready = ticks();
    gov_begin();
    if (ps.kept)
        audio_update();
    c->in_end = f->size;                        /* known from the start (MP3 length estimate) */
    c->comment_end_page = codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS ? cpage : 0;
    c->comment_end_seg = cseg;
    c->seek_ms = seek;
    c->seek_offset = 0;
    if (seek && (codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS)) {
        uint32_t rate = ogg_rate(f);
        uint64_t g = (uint64_t)seek * rate / 1000;
        if (rate && g > rate)                   /* a second before: room to converge */
            c->seek_offset = ogg_page_before(f, g - rate);
        cur.index = cur.cluster = 0;
    }
    if (seek)
        log_printf("player: start at %u ms (Ogg page at %u)", seek, c->seek_offset);
    if (c->comment_end_page)
        log_printf("player: comment header ends at page %u, segment %u", c->comment_end_page,
                   c->comment_end_seg);
    if (codec == HP_CODEC_OPUS || codec == HP_CODEC_VORBIS) {
        uint64_t g = ogg_end_granule(file);
        c->end_granule_lo = (uint32_t)g;
        c->end_granule_hi = (uint32_t)(g >> 32);
        cur.index = cur.cluster = 0;
        log_printf("player: last Ogg granule %u", (uint32_t)g);
    }
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
        audio_start(player.rate, RING_SYSRAM, ps.fb == 4);
        audio_update();
        audio.wr = (audio.rd + 8192) & ~7u;     /* 8 KiB of the zeroed ring, then core B's PCM */
        log_printf("player: core B had %u bytes ready when the DMA started", out_avail());
    }
    if (!ps.kept)
        tail.on = 0;                            /* the DACs started again: nothing of it left */
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
            audio_start(player.rate, RING_SYSRAM, ps.fb == 4);
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
                   audio.ring_ur - ps.ur0, audio.ring_lost - ps.lost0);
    }
    if (!ps.full && audio_fill() >= (int32_t)(audio.ring_size - 8192)) {
        ps.full = 1;                            /* from here, the steady-state minimum fill */
        log_printf("player: DAC ring full after %u ms (core B ring %u bytes)",
                   (ticks() - ps.t_play) / 1000, out_avail());
        if (!ps.kept)
            audio_reset_stats();
    }
    /* frames heard: written by core B minus what is still in the two rings */
    uint32_t queued = (out_avail() + (uint32_t)(audio_fill() > 0 ? audio_fill() : 0)) / ps.fb;
    player.frames = c->start_frame + (c->frames > queued ? c->frames - queued : 0);
    player.b_out = out_avail();
    player.time_ms = player.rate ? (uint32_t)((uint64_t)player.frames * 1000 / player.rate) : 0;
    heard_update();
    player.buffers = c->wakeups;
    if (c->state == HP_ERROR) {
        log_printf("player: core B error %u, fault pc %x lr %x cfsr %x", c->error, c->fault_pc,
                   c->fault_lr, c->fault_cfsr);
        player.error = HIFI_EPROTO;
        return player.error;
    }
    if (c->state == HP_END && !out_avail() &&
        ((ps.flags & PLAY_CHAIN) || audio_fill() <= 4096)) {
        player.state = ST_END;
        return 1;
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
               audio.ring_size / 1024, audio.ring_ur, audio.ring_lost, audio.i2s_ur,
               audio.min_fill, ps.full ? " (since full)" : " (never full)");
    gov_report(own_file.name);
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

/* Blocking form, for the test menus: 'ui' every 20 ms, non-zero to stop */
int player_play_own(const struct fat_file *f, uint32_t codec, int (*ui)(void), int flags)
{
    int r = player_open(f, codec, flags);
    if (r)
        return r;
    int stop = 0;
    uint32_t t_ui = 0;
    while (!(r = player_service())) {
        if (ticks() - t_ui > 20000) {
            t_ui = ticks();
            if (ui()) {
                stop = 1;
                break;
            }
        }
    }
    player_close(stop);
    return player.error ? player.error : stop;
}
