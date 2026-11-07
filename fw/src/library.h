/* The library index NEWFWLIB.DAT (docs/LIBRARY.md, ETUDE §121-§123): read on demand through a
 * few sectors cached, never loaded whole. Any reference out of its section is an error (-1), so
 * that a damaged index cannot make a reader go astray. */
#ifndef LIBRARY_H
#define LIBRARY_H

#include <stdint.h>

/* Where the file's bytes come from: off from the file's start (the header at 0); 0 or < 0 */
struct lib_io {
    int (*read)(void *ctx, uint32_t off, void *buf, uint32_t n);
    void *ctx;
};

enum { LIB_ARTISTS, LIB_ALBUMS, LIB_GENRES, LIB_TRACKS, LIB_COUNTS };

struct lib_artist { uint32_t name, first_album, albums; };
struct lib_album { uint32_t name, first_track, tracks, artist, year; };
struct lib_genre { uint32_t name, first, albums; };
struct lib_track {
    uint32_t cluster, size, mtime, title, file, artist;     /* title, file, artist: strings */
    uint32_t album, genre, track, year, disc, codec, vol, contig;
};

/* Opens the index: its header, the half in use and its CRC, the sections. 0, or < 0: none, or
 * damaged (then every count is 0) */
int lib_open(const struct lib_io *io);
void lib_close(void);
uint32_t lib_count(int what);                   /* LIB_*: 0 when not open */
int lib_artist(uint32_t i, struct lib_artist *a);
int lib_album(uint32_t i, struct lib_album *a);
int lib_genre(uint32_t i, struct lib_genre *g);
int lib_genre_album(const struct lib_genre *g, uint32_t k, uint32_t *album);  /* its k-th */
int lib_song(uint32_t i, uint32_t *track);      /* the i-th track by title */
int lib_track(uint32_t i, struct lib_track *t);
/* A string into buf (max bytes, ended by 0, cut on a character boundary): its length, or < 0 */
int lib_str(uint32_t ref, char *buf, uint32_t max);

/* The order of two strings (UTF-8): < 0, 0, > 0; artist: a leading "The " skipped. The sort of
 * the index (docs/LIBRARY.md): the folded characters, then the bytes; the empty string last */
int lib_cmp(const char *a, const char *b, int artist);

#endif
