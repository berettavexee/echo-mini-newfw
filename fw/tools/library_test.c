/* Host-side test of src/library.c (ETUDE §123): the index made by mklib.py from
 * tools/library_fixture.json, read through library.c, every answer compared with the ones
 * mklib.py expects (--expect); the sort of lib_cmp() against Python's; damaged indexes refused.
 *   python3 tools/mklib.py --json tools/library_fixture.json -o build/lib.dat \
 *       --expect build/lib_expect.txt
 *   cc -O1 -Wall -Isrc -o build/library_test tools/library_test.c && \
 *       build/library_test build/lib.dat build/lib_expect.txt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/library.c"

static uint8_t *img;
static long img_n;
static int fails;

static int img_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    if ((long)off + (long)n > img_n)
        return -1;
    memcpy(buf, img + off, n);
    return 0;
}

static void check(int ok, const char *what, const char *line)
{
    if (!ok) {
        fails++;
        if (fails < 20)
            printf("FAIL %s: %s\n", what, line);
    }
}

static char *str(uint32_t ref)
{
    static char b[4][256];
    static int k;
    k = (k + 1) & 3;
    if (lib_str(ref, b[k], sizeof b[k]) < 0)
        strcpy(b[k], "<bad>");
    return b[k];
}

/* fields of a line split by tabs */
static int split(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (; *s && n < max; s++)
        if (*s == '\t') {
            *s = 0;
            f[n++] = s + 1;
        }
    return n;
}

static int cmp_artist;
static int sort_cmp(const void *a, const void *b)
{
    return lib_cmp(*(char *const *)a, *(char *const *)b, cmp_artist);
}

static void damaged(void)
{
    struct lib_io io = { img_read, 0 };
    uint8_t keep;
    keep = img[100];                            /* the header's CRC */
    img[100] ^= 1;
    check(lib_open(&io) < 0 && lib_count(LIB_TRACKS) == 0, "damaged header refused", "");
    img[100] = keep;
    keep = img[512 + 2000];                     /* a byte of the half in use: its CRC */
    img[512 + 2000] ^= 0x40;
    check(lib_open(&io) < 0, "damaged half refused", "");
    img[512 + 2000] = keep;
    check(lib_open(&io) == 0, "index open again", "");
    struct lib_track t;
    check(lib_track(lib_count(LIB_TRACKS), &t) < 0, "track past the end refused", "");
    struct lib_album a;
    check(lib_album(lib_count(LIB_ALBUMS), &a) < 0, "album past the end refused", "");
    check(lib_str(0xffffff, (char[8]){ 0 }, 8) < 0, "string past the end refused", "");
    char small[6];                              /* "Écrire" cut within 6 bytes: "Écri" */
    struct lib_track e;
    for (uint32_t i = 0; i < lib_count(LIB_TRACKS); i++)
        if (!lib_track(i, &e) && !strcmp(str(e.title), "Écrire")) {
            lib_str(e.title, small, sizeof small);
            check(!strcmp(small, "\xc3\x89" "cri"), "string cut after a whole character",
                  small);
            char tiny[2];                       /* "É" does not fit in 1 byte: nothing */
            lib_str(e.title, tiny, sizeof tiny);
            check(!tiny[0], "string cut inside its first character", tiny);
        }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "library_test LIB.DAT EXPECTED.txt\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    img_n = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc(img_n);
    if (fread(img, 1, img_n, f) != (size_t)img_n)
        return 2;
    fclose(f);
    struct lib_io io = { img_read, 0 };
    if (lib_open(&io)) {
        printf("FAIL lib_open\n");
        return 1;
    }
    FILE *e = fopen(argv[2], "r");
    if (!e)
        return 2;
    static char line[65536];
    int lines = 0;
    while (fgets(line, sizeof line, e)) {
        line[strcspn(line, "\n")] = 0;
        static char copy[65536];
        strcpy(copy, line);
        char *v[64];
        int n = split(line, v, 64);
        lines++;
        if (!strcmp(v[0], "counts")) {
            check(lib_count(LIB_ARTISTS) == strtoul(v[1], 0, 10) &&
                  lib_count(LIB_ALBUMS) == strtoul(v[2], 0, 10) &&
                  lib_count(LIB_GENRES) == strtoul(v[3], 0, 10) &&
                  lib_count(LIB_TRACKS) == strtoul(v[4], 0, 10), "counts", copy);
        } else if (!strcmp(v[0], "artist")) {
            struct lib_artist a;
            check(!lib_artist(strtoul(v[1], 0, 10), &a) && !strcmp(str(a.name), v[2]) &&
                  a.first_album == strtoul(v[3], 0, 10) && a.albums == strtoul(v[4], 0, 10),
                  "artist", copy);
        } else if (!strcmp(v[0], "album")) {
            struct lib_album a;
            check(!lib_album(strtoul(v[1], 0, 10), &a) && !strcmp(str(a.name), v[2]) &&
                  a.artist == strtoul(v[3], 0, 10) && a.year == strtoul(v[4], 0, 10) &&
                  a.first_track == strtoul(v[5], 0, 10) && a.tracks == strtoul(v[6], 0, 10),
                  "album", copy);
        } else if (!strcmp(v[0], "genre")) {
            struct lib_genre g;
            int ok = !lib_genre(strtoul(v[1], 0, 10), &g) && !strcmp(str(g.name), v[2]);
            char got[4096] = "";
            for (uint32_t k = 0; ok && k < g.albums; k++) {
                uint32_t al;
                ok = !lib_genre_album(&g, k, &al);
                sprintf(got + strlen(got), k ? ",%u" : "%u", al);
            }
            check(ok && !strcmp(got, n > 3 ? v[3] : ""), "genre", copy);
        } else if (!strcmp(v[0], "track")) {
            struct lib_track t;
            check(n >= 16 && !lib_track(strtoul(v[1], 0, 10), &t) &&
                  !strcmp(str(t.title), v[2]) && !strcmp(str(t.file), v[3]) &&
                  !strcmp(str(t.artist), v[4]) && t.album == strtoul(v[5], 0, 10) &&
                  t.genre == strtoul(v[6], 0, 10) && t.track == strtoul(v[7], 0, 10) &&
                  t.year == strtoul(v[8], 0, 10) && t.disc == strtoul(v[9], 0, 10) &&
                  t.codec == strtoul(v[10], 0, 10) && t.vol == strtoul(v[11], 0, 10) &&
                  t.contig == strtoul(v[12], 0, 10) && t.cluster == strtoul(v[13], 0, 10) &&
                  t.size == strtoul(v[14], 0, 10) && t.mtime == strtoul(v[15], 0, 10),
                  "track", copy);
        } else if (!strcmp(v[0], "song")) {
            uint32_t t;
            check(!lib_song(strtoul(v[1], 0, 10), &t) && t == strtoul(v[2], 0, 10), "song",
                  copy);
        } else if (!strcmp(v[0], "order")) {    /* Python's order: lib_cmp gives the same */
            cmp_artist = atoi(v[1]);
            int m = n - 2;
            char **s = malloc(sizeof *s * m);
            for (int i = 0; i < m; i++)
                s[i] = v[i + 2];
            qsort(s, m, sizeof *s, sort_cmp);
            int same = 1;
            for (int i = 0; i < m; i++)
                if (s[i] != v[i + 2])
                    same = strcmp(s[i], v[i + 2]) ? (printf("  %d: %s / %s\n", i, s[i],
                                                            v[i + 2]), 0) : same;
            check(same, cmp_artist ? "the order of artists" : "the order of strings", "");
            free(s);
        }
    }
    fclose(e);
    damaged();
    printf("%d lines checked, %d failures\n", lines, fails);
    return fails != 0;
}
