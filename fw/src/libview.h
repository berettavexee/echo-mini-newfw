/* The library's views (ETUDE §127, docs/LIBRARY.md): the lists the screens show, from the index
 * opened by library.c, without what cannot be played: the tracks of a card that is not in, and
 * a track on both volumes shown once (the internal memory's). */
#ifndef LIBVIEW_H
#define LIBVIEW_H

#include <stdint.h>

/* The lists; param: the artist, the year, the genre or the album of the list */
enum {
    LV_ARTISTS = 1,     /* artists: their index */
    LV_ARTIST_ALBUMS,   /* the albums of artist param */
    LV_ALBUMS,          /* all the albums */
    LV_YEARS,           /* the years of the albums (the year itself; 0, no year, last) */
    LV_YEAR_ALBUMS,     /* the albums of year param */
    LV_GENRES,          /* genres */
    LV_GENRE_ALBUMS,    /* the albums of genre param */
    LV_TRACKS,          /* the tracks of album param */
    LV_SONGS,           /* all the tracks, by title */
};

/* After lib_open(): what can be shown worked out. card_in: the card whose tracks the index
 * holds is in (lv_card_serial() tells which). 0, or -1 (a read failed, or more of something
 * than the tables hold) */
int lv_setup(int card_in);
void lv_reset(void);                    /* the index closed or changed: lv_setup() again */
int lv_ready(void);
uint32_t lv_card_serial(void);          /* the card of the index (0: no track on a card) */
uint32_t lv_count(int list, uint32_t param);
int lv_item(int list, uint32_t param, uint32_t i, uint32_t *id);       /* 0, or -1 */
extern void (*lv_yield)(void);          /* called while lv_setup() reads the index, when set */

#endif
