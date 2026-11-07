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
/* The sectors cached: in buf (bytes; 520 a sector), or the four of their own (buf 0) */
void lib_cache(void *buf, uint32_t bytes);
extern void (*lib_yield)(void); /* called while lib_open() checks the CRC, when set */
uint32_t lib_count(int what);                   /* LIB_*: 0 when not open */
int lib_artist(uint32_t i, struct lib_artist *a);
int lib_album(uint32_t i, struct lib_album *a);
int lib_genre(uint32_t i, struct lib_genre *g);
int lib_genre_album(const struct lib_genre *g, uint32_t k, uint32_t *album);  /* its k-th */
int lib_song(uint32_t i, uint32_t *track);      /* the i-th track by title */
int lib_track(uint32_t i, struct lib_track *t);
int lib_volume(uint32_t i, uint32_t *volume, uint32_t *serial, uint32_t *tracks);
/* By volume and first cluster (CLUS): entry i (key vol << 31 | cluster), or the track of a file;
 * 0, or -1 (none) */
int lib_clus(uint32_t i, uint32_t *key, uint32_t *track);
int lib_find(uint32_t volume, uint32_t cluster, uint32_t *track);
/* A string into buf (max bytes, ended by 0, cut on a character boundary): its length, or < 0 */
int lib_str(uint32_t ref, char *buf, uint32_t max);

/* The file on the device (ETUDE §124): NEWFWLIB.DAT at the root of the internal volume, created
 * when missing (fatwrite.c) with a header that names no half. 0 and f: there, the right size and
 * contiguous (created now or before); < 0: not there and not created (FATW_*), or
 * LIB_EDAMAGED: a file of that name that is not one (left as it is) */
#define LIB_FILE     "NEWFWLIB.DAT"
#define LIB_FILE_SIZE (512u + (8u << 20))
#define LIB_EDAMAGED (-10)
struct fat_file;
int lib_file(struct fat_file *f);
void lib_empty_header(uint8_t h[512]);  /* the header of a file with no index yet */
/* CRC-32 (zlib's) going on: start with 0xffffffff, end with ~ */
uint32_t lib_crc(uint32_t c, const void *p, uint32_t n);

/* Our codec for a file name, by its extension (browser.c); 0: not one we play */
uint32_t file_codec(const char *name);
/* n folded characters of s (the sort key's) after the first 'skip', for a sort in RAM: equal
 * prefixes are then refined, or compared with lib_cmp(); 0 past the end; the empty string gives
 * the highest prefix (it sorts last) */
void lib_key(const char *s, int artist, uint16_t *k, int n, int skip);

/* The order of two strings (UTF-8): < 0, 0, > 0; artist: a leading "The " skipped. The sort of
 * the index (docs/LIBRARY.md): the folded characters, then the bytes; the empty string last */
int lib_cmp(const char *a, const char *b, int artist);

#endif
