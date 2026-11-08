/* The play queue's screen (main menu, ETUDE §128): its first line empties the queue (the track
 * playing kept), then the tracks in the queue's order, the one heard marked; VOL+/VOL- move,
 * PLAY plays the track chosen (or empties), PLAY held empties it from any line (a long queue:
 * the first line far up), BACK or MENU closes. The music goes on. */
#include "ui_priv.h"

static int s_sel, s_top, s_dirty, s_shown_cur;
static uint32_t s_msg_at;                       /* "Queue emptied" in the title, 2 s */
static int s_msg;

static int rows(void)
{
    return q_n + 1;
}

static void qs_enter(void)
{
    s_sel = q_n && q_cur < q_n ? q_cur + 1 : 0;     /* on the track heard */
    s_top = s_sel > ROWS / 2 ? s_sel - ROWS / 2 : 0;
    s_dirty = 1;
}

static void qs_shown(void)
{
    s_dirty = 1;
}

static int qs_draw(void)
{
    if (s_msg && millis() - s_msg_at >= 2000) {
        s_msg = 0;
        s_dirty = 1;
    }
    if (!s_dirty && s_shown_cur == q_cur)
        return SCR_STAY;
    lcd_fill(0, 0, LCD_W, LCD_H, BG);
    lcd_text_clip(4, 0, s_msg ? T("Queue emptied") : T("Play queue"), LCD_W - 80,
                  s_msg ? SK_FG : TITLE_FG, BG);
    char b[24], *p = put_dec(b, q_n ? (uint32_t)q_cur + 1 : 0);
    p = put_str(p, "/");
    p = put_dec(p, (uint32_t)q_n);
    *p = 0;
    text_right(LCD_W - 76, LCD_W - 4, 0, b, DIM, BG);
    int n = rows(), xmax = list_text_end(n, ROWS);
    for (int r = 0; r < ROWS && s_top + r < n; r++) {
        int i = s_top + r, y = ROW_Y0 + 16 * r;
        uint16_t bg = i == s_sel ? SEL_BG : BG;
        lcd_fill(0, y, LCD_W, 16, bg);
        if (!i) {
            lcd_text_clip(22, y, T("Empty the queue (the track playing kept)"), xmax, DIR_FG, bg);
            continue;
        }
        if (i - 1 == q_cur && (q_playing || parked))
            lcd_glyph(3, y, 0x266a, TITLE_FG, bg);
        lcd_text_clip(22, y, q_name(i - 1), xmax, i - 1 == q_cur ? TITLE_FG : FG, bg);
    }
    list_scrollbar(ROW_Y0, ROWS, s_top, n);
    s_dirty = 0;
    s_shown_cur = q_cur;
    return SCR_STAY;
}

static int qs_key(const struct key_ev *e)
{
    int ev = e->type, k = e->key;
    if (k == KEY_VOLUP && ev != EV_LONG) {
        list_move(&s_sel, &s_top, rows(), ROWS, -1);
        s_dirty = 1;
    } else if (k == KEY_VOLDOWN && ev != EV_LONG) {
        list_move(&s_sel, &s_top, rows(), ROWS, 1);
        s_dirty = 1;
    } else if (k == KEY_PLAY && ((ev == EV_PRESS && !s_sel) || ev == EV_LONG)) {
        q_clear_keep();                         /* the first line, or held on any line */
        s_sel = s_top = 0;
        s_msg = 1;
        s_msg_at = millis();
        resume_save_later();
        s_dirty = 1;
    } else if (k == KEY_PLAY && ev == EV_PRESS) {
        if (s_sel - 1 < q_n)
            play_index(s_sel - 1);
        resume_save_later();
        s_dirty = 1;
    } else if ((k == KEY_BACK || k == KEY_MENU) && ev == EV_PRESS)
        return SCR_POP;
    return SCR_STAY;
}

const struct screen queue_screen = { qs_enter, qs_shown, qs_key, qs_draw };
