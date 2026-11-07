/* The play screen (ETUDE §25, §39): status, title at twice the size (the full title scrolls when
 * it cannot fit on two lines), artist and album (also scrolled when too long), time and format. */
#include "ui_priv.h"

/* The bar in the seek mode: the position dimmed, the target as a mark above and below it */
static void seek_bar(int y, int h, uint32_t pos, uint32_t target, uint32_t len)
{
    int w = bar_x(pos, len), t = 4 + bar_x(target, len);
    lcd_fill(0, y - 2, LCD_W, 2, BG);
    lcd_fill(0, y + h, LCD_W, 3, BG);
    lcd_fill(4, y, w, h, RGB(60, 90, 140));
    lcd_fill(4 + w, y, LCD_W - 8 - w, h, RGB(40, 40, 50));
    if (t > LCD_W - 6)
        t = LCD_W - 6;
    lcd_fill(t - 1, y - 2, 3, h + 5, SK_FG);
}

/* Battery: the percent shown (ui.c: a mean of a minute, steady), and the USB plug: "CHG" while
 * charging, "USB" once charged */
static int battery_low;                         /* under 3,425 mV without USB: shown in red */

static char *battery_text(char *p)
{
    battery_low = !vbus() && battery_avg_mv() < 3425;    /* = BAT_WARN_MV (ui.c) */
    uint32_t v = battery_shown();
    if (vbus())
        p = put_str(p, charging() ? EN("CHG ") : EN("USB "));
    p = put_dec(p, v);
    return put_str(p, EN("%"));
}

static const char *codec_name(uint32_t c)
{
    static const char *const n[] = { "", "WAV", "FLAC", "MP3", "Vorbis", "Opus", "AIFF", "AAC", "ALAC",
                                       "MPC" };          /* names: the same in every language */
    return c < sizeof n / sizeof *n ? n[c] : "";
}

/* Lines of the play screen too long for it (artist, album) scroll left: 1.5 s still at the
 * start, then 1 pixel every 33 ms (30 px/s), round and round with a gap of 32 pixels */
#define MQ_X     4
#define MQ_XMAX  (LCD_W - 4)
static struct marquee {
    const char *s;
    int y, w, off, sc;
    uint16_t fg;
    uint32_t t;                                 /* start of the pause, then of the last step */
} mq[3];

static void marquee_set(struct marquee *m, int y, const char *s, uint16_t fg, int sc)
{
    m->s = s;
    m->y = y;
    m->fg = fg;
    m->sc = sc;
    m->off = 0;
    m->t = ticks();
    m->w = lcd_text_width_x(s, sc);
    if (m->w <= MQ_XMAX - MQ_X)
        m->w = 0;                               /* it fits: drawn once, never scrolled */
    lcd_text_clip_x(MQ_X, y, s, MQ_XMAX, fg, BG, sc);
}

void marquee_tick(void)
{
    uint32_t now = ticks();
    int paused = !q_playing || audio_paused;    /* paused, parked or stopped: the text stands still */
    for (int i = 0; i < 3; i++) {
        struct marquee *m = &mq[i];
        if (paused) {
            m->t = now;                         /* frozen where it is: no jump on resume */
            continue;
        }
        if (!m->w || now - m->t < (m->off ? MQ_STEP : MQ_PAUSE))
            continue;
        m->off += m->off ? (int)((now - m->t) / MQ_STEP) : 1;
        m->t = now;
        if (m->off >= m->w + MQ_GAP)
            m->off = 0;                         /* back at the start: still again */
        lcd_text_scroll(MQ_X, m->y, m->s, MQ_XMAX, m->off, MQ_GAP, m->sc, m->fg, BG);
    }
}

static void draw_playing_do(int full)
{
    char b[96], *p;
    if (!q_playing && !q_n) {
        if (full) {
            lcd_fill(0, 0, LCD_W, LCD_H, BG);
            lcd_text(8, 60, T("nothing plays: PLAY on a file"), DIM, BG);
        }
        return;
    }
    /* Each band is drawn over the previous picture, every pixel written once: no clearing first,
     * which flickered (ETUDE §39). A full drawing also paints the gaps between the bands. */
    if (full) {
        mq[0].w = mq[1].w = mq[2].w = 0;       /* set again below, with the new texts */
        lcd_fill(0, 16, LCD_W, 2, BG);
        lcd_fill(0, 82, LCD_W, 4, BG);
        lcd_fill(0, 118, LCD_W, 4, BG);
        lcd_fill(0, 166, LCD_W, LCD_H - 166, BG);
        lcd_fill(0, 0, 4, 16, BG);
        lcd_fill(0, 18, 4, 64, BG);             /* the margins of the title, artist, album, */
        lcd_fill(LCD_W - 4, 18, 4, 64, BG);     /* time and info lines */
        lcd_fill(0, 86, 4, 32, BG);
        lcd_fill(LCD_W - 4, 86, 4, 32, BG);
        lcd_fill(0, 122, 4, 16, BG);
        lcd_fill(0, 150, 4, 16, BG);
        lcd_fill(0, 138, LCD_W, 2, BG);         /* around the bar */
        lcd_fill(0, 140, 4, 5, BG);
        lcd_fill(LCD_W - 4, 140, 4, 5, BG);
        lcd_fill(0, 145, LCD_W, 5, BG);
    }
    /* status: playing / paused, position in the queue */
    static const char *const modes[MODE_COUNT] = { "", N_("repeat all"), N_("repeat one"),
                                                   N_("shuffle") };
    p = log_format(b, b + sizeof b - 1, EN("%s  %u/%u%s%s"),
                   !q_playing ? (parked ? T("paused") : T("stopped")) :
                   audio_paused ? T("paused") : T("playing"),
                   (uint32_t)q_cur + 1, (uint32_t)q_n, play_mode ? "  " : "", tr(modes[play_mode]));
    *p = 0;
    lcd_text_clip(4, 0, b, LCD_W - 104, TITLE_FG, BG);
    draw_clock(LCD_W - 104, LCD_W - 68, full);
    p = battery_text(b);
    *p = 0;
    text_right(LCD_W - 68, LCD_W, 0, b, battery_low ? WARN : DIM, BG);
    if (full) {
        /* keep two lines when the full title fits; otherwise scroll the complete title in one
         * large line, like a conventional MP3 player marquee */
        const char *nm = tg.title[0] && !cfg.names ? tg.title : q_name(q_cur), *s = nm, *s_sp = 0;
        char l1[96];
        int w = 0, n = 0, n_sp = 0;
        while (*s) {
            const char *t = s;
            uint32_t c = utf8_next(&t);
            char one[5] = { 0 };
            for (int k = 0; k < t - s && k < 4; k++)
                one[k] = s[k];
            int cw = lcd_text_width_x(one, 2);
            if (w + cw > LCD_W - 8 || n + (t - s) >= (int)sizeof l1 - 1)
                break;
            for (const char *k = s; k < t; k++)
                l1[n++] = *k;
            w += cw;
            s = t;
            if (c == ' ')
                n_sp = n, s_sp = s;
        }
        if (*s && s_sp && n_sp > n / 2)         /* a break between words */
            n = n_sp, s = s_sp;
        l1[n] = 0;
        if (lcd_text_width_x(s, 2) > LCD_W - 8) {
            lcd_fill(4, 18, LCD_W - 8, 64, BG);
            marquee_set(&mq[0], 34, nm, FG, 2);
        } else {
            lcd_text_clip_x(4, 18, l1, LCD_W - 4, FG, BG, 2);
            lcd_text_clip_x(4, 50, s, LCD_W - 4, FG, BG, 2);
        }
        marquee_set(&mq[1], 86, cfg.names ? "" : tg.artist, TITLE_FG, 1); /* names: the folder */
        marquee_set(&mq[2], 102, tg.album[0] && !cfg.names ? tg.album : q_folder, DIR_FG, 1);
    }
    if (sk_on) {                                /* the target, how far, what the keys do */
        p = put_str(b, T("go to"));
        *p++ = ' ';
        p = fmt_time(p, sk_ms);
        p = put_str(p, " / ");
        p = fmt_time(p, len_ms());
        p = put_str(p, sk_ms >= sk_from ? "   +" : "   -");
        p = fmt_time(p, sk_ms >= sk_from ? sk_ms - sk_from : sk_from - sk_ms);
        lcd_text_clip(4, 122, b, LCD_W, SK_FG, BG);
        seek_bar(140, 5, now_ms(), sk_ms, len_ms());
        lcd_text_clip(4, 150, T("VOL+/- move  PLAY go  BACK cancel"), LCD_W, DIM, BG);
        return;
    }
    p = fmt_time(b, now_ms());
    p = put_str(p, " / ");
    p = fmt_time(p, len_ms());
    *p = 0;
    lcd_text_clip(4, 122, b, LCD_W, FG, BG);
    progress_bar(140, 5, now_ms(), len_ms());
    p = put_str(b, codec_name(q_ent(q_cur) ? q_ent(q_cur)->flags & F_CODEC : 0));
    p = put_str(p, "  ");
    p = put_dec(p, player_heard.rate / 1000);
    if (player_heard.rate % 1000) {
        *p++ = '.';
        p = put_dec(p, player_heard.rate % 1000 / 100);
    }
    p = put_str(p, EN(" kHz  "));
    p = put_dec(p, player_heard.bps);
    p = put_str(p, EN(" bit   -"));
    p = put_dec(p, (uint32_t)play_level / 2);
    p = put_str(p, EN(" dB  "));
    *p = 0;
    /* left to right, each pixel once: the text, the ReplayGain box (on yellow when the gain is
     * the track's), then the rest of the line */
    int x = lcd_text(4, 150, b, FG, BG);
    if (rg_mode != RG_MODE_OFF) {
        p = put_str(b, EN(" RG "));
        if (rg_tag == RG_NONE)
            p = put_str(p, T("none"));
        else {
            unsigned a = (unsigned)(rg_steps < 0 ? -rg_steps : rg_steps);
            *p++ = rg_steps < 0 ? '-' : '+';
            p = put_dec(p, a / 2);
            *p++ = '.';
            *p++ = a % 2 ? '5' : '0';
        }
        p = put_str(p, " ");
        *p = 0;
        int track = rg_tag != RG_NONE && rg_src == RG_TRACK;
        x = lcd_text(x, 150, b, track ? BG : FG, track ? RG_TRACK_BG : BG);
    }
    if (x < LCD_W)
        lcd_fill(x, 150, LCD_W - x, 16, BG);
}

void draw_playing(int full)
{
    const char *pp = perf_begin("draw");
    draw_playing_do(full);
    perf_end(pp);
}

/* A key on the play screen: VOL+/VOL- volume, PLAY pause / play, PLAY held next track (stop past
 * the last), BACK start of the track or the one before, BACK held the play mode. Returns UI_*. */
int play_key(const struct key_ev *e)
{
    int k = e->key, ev = e->type;
    if (key_is_vol(k))
        volume((k == KEY_VOLUP) != !!cfg.swap_vol ? -2 : 2);
    else if (k == KEY_PLAY && ev == EV_PRESS) {
        if (q_playing) {
            audio_pause(!audio_paused);
            resume_save_later();
        }
        else if (q_n) {
            if (parked)
                player_seek_hint(parked_ms);
            play_index(q_cur);
        }
    } else if (k == KEY_PLAY && ev == EV_LONG) {
        int n = q_next_index(q_cur, 1);
        if (n >= 0)
            play_index(n);
        else if (q_n && (q_playing || parked)) {
            play_stop(STOP_NOW);                /* past the end of the queue: stopped there, */
            parked = 0;                         /* PLAY plays the last track from its start */
            player_heard.time_ms = 0;
            log_printf("ui: end of the queue (next on the last track)");
            resume_save_later();
        }
    } else if (k == KEY_BACK && ev == EV_PRESS) {
        if (q_n) {
            int prev = q_prev_index(q_cur);     /* shuffle: the one played before */
            if (player_heard.time_ms > 3000 || prev < 0)
                play_index(q_cur);              /* back to the start of the track */
            else
                play_step(prev, -1);
        }
    } else if (k == KEY_BACK && ev == EV_LONG) {
        play_mode = (play_mode + 1) % MODE_COUNT;
        rnd ^= ticks();
        if (play_mode == MODE_SHUFFLE)
            q_shuffle_new(q_cur);
        rg_apply();                             /* auto: the album's, the track's in shuffle */
        resume_save_later();
    }
    return UI_QUICK;                            /* the bands redrawn, not the title: a new track */
}                                               /* sets a full drawing (ui_main) */

/* A key while seeking: VOL+/VOL- move the target, PLAY or MENU go there, BACK cancels */
int seek_key(const struct key_ev *e)
{
    if (key_is_vol(e->key)) {
        seek_move(e->key == KEY_VOLUP ? 1 : -1, e->type == EV_REPEAT);
        return UI_QUICK;
    }
    if (e->type != EV_PRESS)
        return UI_QUICK;
    seek_end(e->key != KEY_BACK);               /* PLAY, MENU: go there; BACK: cancel */
    return UI_QUICK | UI_FULL;
}
