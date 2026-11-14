/* The library's index built on the device (ETUDE §125-§126): libbuild.c */
#ifndef LIBBUILD_H
#define LIBBUILD_H

#include <stdint.h>

enum { LB_COUNT, LB_SCAN, LB_SORT, LB_WRITE };  /* phases, for the progress */
enum { LB_ECANCEL = -20, LB_ETRACKS = -21, LB_ENAMES = -22, LB_EDIRS = -23, LB_EFULL = -24,
       LB_EIO = -25, LB_EFILE = -26 };

struct lb_hooks {
    /* every 100 ms or so: the phase, done of total (files, or the steps of a phase); returns
     * nonzero to stop (the index before kept) */
    int (*progress)(int phase, uint32_t done, uint32_t total);
};

struct lb_result {
    uint32_t tracks, read, reused, artists, albums, genres, bytes, ms;
};

/* Builds the index of the internal volume and of the card (when mounted) into the half of
 * NEWFWLIB.DAT not in use, then makes it the one in use: 0, or < 0 (LB_*, the index before
 * kept). Uses core B's memory (HIGHRAM) and the input ring: nothing may play (the caller stops
 * the player); core B's codec has to be loaded again after it. The file is created when
 * missing (lib_file). Blocking: the hooks keep the screen and the watchdog going. */
int lib_build(const struct lb_hooks *h, struct lb_result *r);

/* 48 KiB of core B's memory that lib_build() used, still powered and free when it returns (until
 * a codec is loaded again): the free space counted in it (libui.c) */
#define LB_WORK       ((void *)0x0302c000u)
#define LB_WORK_BYTES (48u * 1024)

#endif
