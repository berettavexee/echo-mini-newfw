/* Read-only FAT32 on the internal user volume, FAT32 or exFAT on the microSD card (0.57,
 * ETUDE §37). Files and directories carry their volume: FAT_VOL_INT or FAT_VOL_SD. */
#ifndef FAT_H
#define FAT_H

#include <stdint.h>

enum { FAT_VOL_INT, FAT_VOL_SD };

struct fat_vol {
    uint8_t dev;                /* FAT_VOL_INT: the eMMC, FAT_VOL_SD: the card */
    uint8_t exfat;
    uint8_t mounted;
    uint32_t base;              /* first sector of the volume (MBR or boot sector) */
    uint32_t part;              /* boot sector */
    uint32_t fat;               /* first FAT */
    uint32_t data;              /* cluster 2 */
    uint32_t spc;               /* sectors per cluster */
    uint32_t clusters;
    uint32_t root;              /* first cluster of the root directory */
};

struct fat_file {
    char name[13];              /* 8.3 name, "NAME.EXT" (exFAT: the start of the name) */
    uint8_t vol;                /* FAT_VOL_INT or FAT_VOL_SD */
    uint8_t contig;             /* exFAT "no FAT chain": the clusters follow each other */
    uint32_t cluster;
    uint32_t size;
    uint32_t pos;               /* read position, bytes */
    uint32_t cur;               /* cluster holding pos */
};

extern struct fat_vol vol;      /* the internal volume */
extern struct fat_vol sdvol;    /* the card's */

int fat_mount(uint32_t from);   /* searches from sector 'from' in 1 MiB steps */
int fat_mount_sd(void);         /* the first FAT32 or exFAT partition of the card (sd_init done) */
/* Calls fn for each regular file of the root directory; stops when fn returns non-zero */
int fat_list_root(int (*fn)(const struct fat_file *f, void *ctx), void *ctx);
void fat_open(struct fat_file *f);
/* Reads up to n bytes (multiple of 512, from a 512-aligned position); returns the count or <0 */
int fat_read(struct fat_file *f, void *buf, uint32_t n);

enum { FAT_DIRTY_BS = 1, FAT_DIRTY_FAT = 2, FAT_DIRTY_ERR = 4 };
int fat_dirty(void);            /* mask of the marks above, <0 on a read error */
int fat_find_root(const char *name, struct fat_file *f);    /* 8.3 name in the root, 0 = found */

/* Reads n bytes at any position (pos + n within the file); a cursor caches the last cluster
 * so that sequential reads do not walk the chain from the start. Returns n or < 0. */
struct fat_cursor {
    uint32_t index, cluster;    /* cluster number 'index' of the file is 'cluster' */
};
int fat_pread(const struct fat_file *f, struct fat_cursor *c, uint32_t pos, void *buf, uint32_t n);
int fat_extent(const struct fat_file *f, uint32_t *first, uint32_t *count);  /* contiguous only */

/* Directory listing with long names (VFAT): 'name' is the long name in UTF-8 (cut to
 * FAT_NAME_MAX - 1 bytes on a character boundary), or the 8.3 name when there is none; '.', '..',
 * the volume label, hidden or system entries and names starting with '.' are left out.
 * cluster 0 = the root. fn returns non-zero to stop. Returns 0, or < 0 on a read error. */
#define FAT_NAME_MAX 128
#define FAT_ATTR_DIR 0x10
struct fat_dirent {
    char name[FAT_NAME_MAX];
    char sname[13];             /* 8.3 name (exFAT: the start of the name) */
    uint8_t attr;
    uint8_t vol, contig;
    uint32_t cluster, size;
    uint32_t mtime;             /* last write, FAT date << 16 | FAT time (local time of the PC) */
};
int fat_list_dir(uint32_t cluster, int (*fn)(const struct fat_dirent *e, void *ctx), void *ctx);

/* A directory of either volume: cluster 0 = its root; exFAT directories also need their size
 * and "no FAT chain" flag. Files of 4 GiB or more are left out. */
struct fat_dir {
    uint32_t cluster, size;
    uint8_t vol, contig;
};
int fat_list(const struct fat_dir *d, int (*fn)(const struct fat_dirent *e, void *ctx), void *ctx);
void fat_dir_of(const struct fat_dirent *e, struct fat_dir *d);
void fat_root(int vol, struct fat_dir *d);
/* A fat_file to read the entry e (a regular file) */
void fat_file_of(const struct fat_dirent *e, struct fat_file *f);

#endif
