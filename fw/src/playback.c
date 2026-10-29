/* Playback from the queue (ETUDE §25 onwards): opening tracks (tags, ReplayGain), chaining them
 * gapless from the main loop, skips and stops, the parked state of a long pause, the volume and
 * the seek on the bar. The screen and the resume state follow the track heard (heard_sync). */
#include "ui_priv.h"

/* ---- playback ---- */
int play_level = 80;                            /* PCM_VOL: attenuation / 2 dB, start at -40 dB */
struct tags tg;                                 /* of the track heard */
static struct tags tg_next;                     /* of the one opened last */
static int fresh;                               /* the output was (re)started: fade in once fed */
static int draining;                            /* end of the queue: the DAC ring plays out */
uint32_t heard_gen;

/* ReplayGain (ETUDE §35): through the DAC volume (0.5 dB steps, no work on the samples), from the
 * moment the track is heard. Album mode takes the track gain when there is no album gain; no tag:
 * 0 dB; no preamp. Limited by the peak (when tagged) and by 0 dB of attenuation. */
const char *const rg_names[RG_MODES] = { "album", "track", "off" };
int rg_mode;
int rg_steps;                                   /* applied to the track heard, 0.5 dB (+: louder) */
int rg_tag = RG_NONE;                           /* its tag, 1/100 dB */
int rg_src;                                     /* RG_TRACK or RG_ALBUM: the gain used */

static int out_level(void)                      /* the DAC attenuation: the volume, then the gain */
{
    int v = play_level - rg_steps;
    return v < 0 ? 0 : v > 0xfe ? 0xfe : v;
}

void rg_apply(void)
{
    int g = RG_NONE, steps = 0, a = RG_ALBUM;
    uint32_t pk = 0;
    if (rg_mode != RG_MODE_OFF) {
        a = rg_mode == RG_MODE_ALBUM && tg.rg_gain[RG_ALBUM] != RG_NONE ? RG_ALBUM : RG_TRACK;
        g = tg.rg_gain[a];
        pk = tg.rg_peak[a] ? tg.rg_peak[a] : tg.rg_peak[a ^ 1];
    }
    if (g != RG_NONE) {
        steps = (g >= 0 ? g + 25 : g - 25) / 50;
        if (pk) {                               /* headroom of the peak, 0.5 dB steps */
            uint32_t x = pk;
            int h = 0;
            if (x <= 65536)
                while (h < 120 && (uint64_t)x * 69419 >> 16 <= 65536) {    /* x 10^(0.5/20) */
                    x = (uint32_t)((uint64_t)x * 69419 >> 16);
                    h++;
                }
            else
                while (x > 65536 && h > -120) {
                    x = (uint32_t)(((uint64_t)x << 16) / 69419);
                    h--;
                }
            if (steps > h)
                steps = h;
        }
    }
    if (steps != rg_steps || g != rg_tag) {
        unsigned m = (unsigned)(steps < 0 ? -steps : steps);
        log_printf("ui: replaygain %s: %s tag %d/100 dB, peak %u/65536: %s%u.%u dB applied",
                   rg_names[rg_mode], a == RG_TRACK ? "track" : "album", g, pk,
                   steps < 0 ? "-" : "+", m / 2, m % 2 * 5);
        rg_steps = steps;
        rg_tag = g;
    }
    rg_src = g == RG_NONE ? RG_ALBUM : a;
    if (!audio.on || audio.vol == 0xff)
        return;                                 /* muted until the fade in: it takes out_level() */
    if (audio_paused)
        audio.vol = (uint8_t)out_level();       /* audio_pause(0) puts it back */
    else if (audio.vol != out_level())
        audio_volume((uint8_t)out_level(), 1);
}

/* A chained track is opened up to 1.1 s before it is heard (the DAC ring plays the end of the
 * last one meanwhile): the screen, the resume state and the keys follow the track heard */
static void heard_sync(void)
{
    if (player_heard.gen == heard_gen)
        return;
    heard_gen = player_heard.gen;
    q_cur = q_dec;
    tg = tg_next;
    rg_apply();
    resume_save();
}

/* Why nothing could be played: shown by the main loop (a banner, then the browser) */
char fail_name[64];
int fail_code, fail_due;
uint32_t fail_why;

void play_failed(const char *name, int code)
{
    const char *what, *hint;
    copy_name(fail_name, name, sizeof fail_name);
    fail_code = code;
    fail_why = code <= PLAYER_EFORMAT && code >= PLAYER_EFAULT ? player.why : HP_WHY_NONE;
    fail_due = 1;
    fail_text(code, fail_why, &what, &hint);
    log_printf("error: %s: %s (code %d, why %u)", fail_name, what, code, fail_why);
}

static const char *const why_text[] = {
    [HP_WHY_NONE] = "Not supported: this variant of the format",
    [HP_WHY_CHANNELS] = "Not supported: more than 2 channels",
    [HP_WHY_BITS] = "Not supported: this bit depth",
    [HP_WHY_FLOAT] = "Not supported: floating-point samples",
    [HP_WHY_COMPRESSED] = "Not supported: compressed WAV or AIFF",
    [HP_WHY_HEAAC] = "Not supported: HE-AAC (AAC+)",
    [HP_WHY_AAC_TYPE] = "Not supported: this AAC profile",
    [HP_WHY_MP4_CODEC] = "Not supported: neither AAC nor ALAC",
    [HP_WHY_OPUS_MAPPING] = "Not supported: multichannel Opus",
    [HP_WHY_MEMORY] = "Not supported: too large for the memory",
    [HP_WHY_RATE] = "Not supported: no sample rate",
};

void fail_text(int code, uint32_t why, const char **what, const char **hint)
{
    static const char check[] = "Check the file on a computer";
    *hint = check;
    switch (code) {
    case Q_EEMPTY:
        *what = "Empty file (0 bytes)";
        *hint = "Copy it again";
        break;
    case PLAYER_ENOAUDIO:
        *what = "Damaged: no audio found";
        break;
    case PLAYER_EFORMAT:
        *what = "Damaged, or not what its name says";
        break;
    case PLAYER_EDECODE:
        *what = "Damaged: decoding error";
        break;
    case PLAYER_EUNSUPPORTED:
        *what = why_text[why < sizeof why_text / sizeof *why_text ? why : HP_WHY_NONE];
        *hint = why == HP_WHY_CHANNELS ? "Convert it to stereo" :
                why == HP_WHY_HEAAC || why == HP_WHY_AAC_TYPE ? "Convert it to AAC-LC, MP3 or Opus" :
                "Convert it to FLAC, MP3, AAC-LC or Opus";
        break;
    case HIFI_EREAD:
        *what = "Read error";
        *hint = "Reinsert the card, or check the disk";
        break;
    case HIFI_EIMG:
        *what = "Its codec is missing from the firmware";
        *hint = "Install the firmware again";
        break;
    default:                                    /* timeout, fault, protocol */
        *what = "Internal error (core B)";
        *hint = "Please report it with NEWFWLOG.TXT";
    }
}

static int q_open(int i, int chain)
{
    struct fat_file f;
    const struct entry *e = q_ent(i);
    if (!e)
        return HIFI_EREAD;                      /* its folder not readable (or changed) */
    if (!e->size)
        return Q_EEMPTY;                        /* not queued since 0.65; kept for safety */
    if ((e->flags & F_SD) && sd_open())
        return HIFI_EREAD;                      /* the card forgotten (USB) and not readable */
    uint16_t flags = e->flags;
    copy_name(f.name, q_name(i), sizeof f.name);
    f.vol = flags & F_SD ? FAT_VOL_SD : FAT_VOL_INT;
    f.contig = (flags & F_CONTIG) != 0;
    f.cluster = e->cluster;
    f.size = e->size;
    fat_open(&f);
    uint32_t t0 = ticks();
    tags_read(&f, flags & F_CODEC, &tg_next);
    log_printf("ui: tags in %u us: '%s' / '%s' / '%s' / %u", ticks() - t0, tg_next.title,
               tg_next.artist, tg_next.album, tg_next.track);
    int was_on = audio.on;
    (void)chain;
    player_ogg_hint(tg_next.ogg_end_page, tg_next.ogg_end_seg);  /* core B skips the comment header */
    int r = player_open(&f, flags & F_CODEC, PLAY_CHAIN);
    if (!r && (!was_on || audio.vol == 0xff))
        fresh = 1;
    log_printf("ui: queue %d/%d: %s (%d)", i + 1, q_n, q_name(i), r);
    if (!r) {
        q_set_codec(i, player.codec);           /* M4A: AAC or ALAC */
        q_dec = i;
        heard_sync();                           /* at once unless the last one still plays */
    }
    return r;
}

/* Parked: a long pause (PARK_MS) stops the track, core B and the DACs; PLAY starts it again
 * where it was (player_seek_hint). Also the state after a resume of a paused track. */
int parked;
uint32_t parked_ms;

void play_stop(int how)
{
    draining = 0;
    if (how != STOP_DRAIN)
        player_drop_tail();
    if (player_is_open())
        player_close(how == STOP_SKIP ? 0 : 1);
    if (how == STOP_NOW && audio_paused) {      /* from a pause: stopped as it is, without */
        audio_stop();                           /* powering the DACs up and unmuting them */
        audio_paused = 0;
        q_playing = 0;
        return;
    }
    audio_pause(0);
    if (how == STOP_SKIP) {
        audio_flush();
        return;
    }
    if (how == STOP_DRAIN)
        player_chain_end();
    audio_stop();
    q_playing = 0;
}

/* Plays track i of the queue; one that does not open is passed over in the direction step (+1, or
 * -1 for BACK: a failing track before does not send it forward again) */
void play_step(int i, int step)
{
    int keep = q_playing && audio.on;
    parked = 0;
    if (player_is_open() || keep)
        play_stop(keep ? STOP_SKIP : STOP_NOW);
    q_cur = q_dec = i;
    q_playing = 1;
    q_errors = 0;
    if (!q_n) {
        q_playing = 0;
        return;
    }
    int r, r0 = 0;
    uint32_t why0 = 0;
    while ((r = q_open(q_dec, keep)) < 0) {
        if (q_dec == i) {                       /* the screen tells why the one chosen failed */
            r0 = r;
            why0 = player.why;
        }
        q_dec += step;
        if (++q_errors > 8 || q_dec < 0 || q_dec >= q_n) {
            play_stop(STOP_NOW);
            q_cur = q_dec = i;                  /* none would open: stay on the one chosen */
            player.why = why0;
            play_failed(q_name(i), r0);
            return;
        }
    }
    q_cur = q_dec;                              /* chosen: shown at once, even while the DACs */
    tg = tg_next;                               /* change rate (published after it) */
    rg_apply();
}

void play_index(int i)
{
    play_step(i, 1);
}

/* The player between two key polls; called during long reads too */
void ui_background(void)
{
    if (!q_playing)
        return;
    if (fresh && audio.on && audio.vol == 0xff && audio_fill() > 16384) {
        fresh = 0;
        audio_volume((uint8_t)out_level(), 1);  /* fade in once a little audio is queued */
    }
    if (draining) {                             /* the end of the queue, heard to its last frame */
        if (!player_drain_service())
            return;
        play_stop(STOP_DRAIN);
        q_cur = q_dec = q_n ? q_n - 1 : 0;
        return;
    }
    int r = player_service();
    heard_sync();
    if (r == 0)
        return;
    if (r == 1 && audio_paused && play_mode == MODE_ORDER && q_dec + 1 >= q_n)
        return;                                 /* end of the queue in a pause: drained after it */
    player_close(0);                            /* end of the track (or an error) */
    if (r == 1 || player.time_ms > 10000)
        q_errors = 0;                           /* it played: only failures in a row stop */
    int first = q_dec, err = r < 0 ? r : 0;     /* the first that failed, why */
    for (int n = q_next_index(q_dec, 0); n >= 0; n = q_next_index(n, 0)) {
        q_dec = n;
        int o = q_open(q_dec, 1);
        if (o == 0)
            return;                             /* the next one, gapless when it can */
        if (!err) {
            first = q_dec;
            err = o;
        }
        if (++q_errors > 8)
            break;
    }
    if (err)                                    /* the queue stops on a failure: said */
        play_failed(q_name(first), err);
    draining = 1;                               /* end of the queue: followed from the loop */
}

/* ---- seek on the bar (ETUDE §31): VOL+ and VOL- together on the play screen. VOL+/VOL- move a
 * target (10 s a step; held: 30 s after 1.5 s, 1 min after 4 s, at most 4 steps a second); PLAY
 * or MENU goes there, BACK cancels, SEEK_IDLE_MS without a key goes there too. The music goes on
 * meanwhile: one reopen at the end (player_seek_hint), whatever the number of steps. From a
 * pause the track plays again there; parked, only the position kept changes. ---- */
int sk_on, sk_cur;
uint32_t sk_ms, sk_from, sk_key_ms;
static uint32_t sk_step_ms;

uint32_t now_ms(void)                    /* the position shown */
{
    return parked ? parked_ms : player_heard.time_ms;
}

void seek_begin(void)
{
    if (!q_n || !player_heard.length_ms || (!q_playing && !parked)) {
        log_printf("ui: no seek (nothing open, or no length)");
        return;
    }
    sk_on = 1;
    sk_cur = q_cur;
    sk_ms = sk_from = now_ms();
    sk_key_ms = millis();
    sk_step_ms = 0;
}

void seek_move(int d, int repeat)
{
    uint32_t now = millis(), held = key_held_us() / 1000;
    sk_key_ms = now;
    if (repeat && now - sk_step_ms < 250)
        return;
    sk_step_ms = now;
    uint32_t step = !repeat || held < 1500 ? 10000 : held < 4000 ? 30000 : 60000;
    uint32_t max = player_heard.length_ms > 1000 ? player_heard.length_ms - 1000 : 0;
    if (d < 0)
        sk_ms = sk_ms > step ? sk_ms - step : 0;
    else
        sk_ms = sk_ms + step < max ? sk_ms + step : max;
}

void seek_end(int go)
{
    sk_on = 0;
    uint32_t d = sk_ms > sk_from ? sk_ms - sk_from : sk_from - sk_ms;
    if (!go || d < 500 || sk_cur != q_cur || (!q_playing && !parked))
        return;
    log_printf("ui: seek %u -> %u ms of %u%s", now_ms(), sk_ms, player_heard.length_ms,
               parked ? " (parked)" : "");
    if (parked) {
        parked_ms = sk_ms;
        resume_save();
        return;
    }
    player_seek_hint(sk_ms);
    play_index(q_cur);
}

void volume(int d)
{
    play_level += d;
    if (play_level < 0)
        play_level = 0;
    if (play_level > 0xfe)
        play_level = 0xfe;
    if (audio.on && audio.vol != 0xff && !audio_paused)
        audio_volume((uint8_t)out_level(), 0);
}
