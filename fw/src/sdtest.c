/* Emulator test of the microSD card and of the FAT32/exFAT reader (ETUDE §36-37), built only with
 * make EXTRA=-DSD_WALK: at boot the card is set up and mounted, its root listed in the log, every
 * folder down to depth 3 walked and every file read and hashed (FNV-1a over 32-bit words), to
 * compare with the files of the image (fw/tools/exfat_add.py). */
#ifdef SD_WALK
#include <stdint.h>
#include "board.h"
#include "fat.h"
#include "log.h"
#include "sd.h"

/* Emulator test of the card (ETUDE §36-37, make EXTRA=-DSD_WALK): set up, mounted, its root
 * listed in the log, every folder walked and every file hashed (sd_walk) */
static int sd_root_entry(const struct fat_dirent *e, void *ctx)
{
    int *n = ctx;
    if (*n < 12)
        log_printf("sd:   %s%s, %u bytes, cluster %u%s", e->name, e->attr & FAT_ATTR_DIR ? "/" : "",
                   e->size, e->cluster, e->contig ? " (contiguous)" : "");
    (*n)++;
    return 0;
}

/* every directory down to depth 3 listed, every file read through fat_pread and hashed (FNV-1a
 * over 32-bit words) */
static struct fat_dirent walk_e[8];
static int walk_n[4];

static int walk_cb(const struct fat_dirent *e, void *ctx)
{
    int depth = *(int *)ctx;
    if (walk_n[depth] < 8)
        walk_e[walk_n[depth]] = *e;
    walk_n[depth]++;
    return 0;
}

static void sd_walk(const struct fat_dir *d, int depth)
{
    static uint32_t buf[512];
    static struct fat_dirent list[4][8];
    walk_n[depth] = 0;
    if (fat_list(d, walk_cb, &depth))
        log_printf("sd: walk: read error");
    int n = walk_n[depth] < 8 ? walk_n[depth] : 8;
    for (int i = 0; i < n; i++)
        list[depth][i] = walk_e[i];
    log_printf("sd: walk depth %d: %d entries", depth, walk_n[depth]);
    for (int i = 0; i < n; i++) {
        const struct fat_dirent *e = &list[depth][i];
        if (e->attr & FAT_ATTR_DIR) {
            log_printf("sd: walk dir %s", e->name);
            struct fat_dir sub;
            fat_dir_of(e, &sub);
            if (depth < 3)
                sd_walk(&sub, depth + 1);
            continue;
        }
        struct fat_file f;
        struct fat_cursor c = { 0, 0 };
        fat_file_of(e, &f);
        uint32_t h = 2166136261u, pos = 0;
        while (pos < f.size) {
            uint32_t k = f.size - pos > sizeof buf ? sizeof buf : f.size - pos;
            for (uint32_t j = 0; j < sizeof buf / 4; j++)
                buf[j] = 0;
            if (fat_pread(&f, &c, pos, buf, k) != (int)k) {
                log_printf("sd: walk %s: read error at %u", e->name, pos);
                break;
            }
            for (uint32_t j = 0; j < (k + 3) / 4; j++)
                h = (h ^ buf[j]) * 16777619u;
            pos += k;
        }
        log_printf("sd: walk file %s: %u bytes, hash %x", e->name, f.size, h);
    }
}

void sd_test(void)
{
    if (!sd_present()) {
        log_printf("sd: no card");
        return;
    }
    uint32_t t0 = ticks();
    if (sd_init())
        return;
    uint32_t t1 = ticks();
    if (fat_mount_sd()) {
        log_printf("sd: no FAT32 or exFAT volume found");
        return;
    }
    log_printf("sd: ready in %u ms; %s volume at %u, %u clusters of %u sectors, FAT %u, "
               "data %u, root cluster %u (mounted in %u ms)", (t1 - t0) / 1000,
               sdvol.exfat ? "exFAT" : "FAT32", sdvol.part, sdvol.clusters, sdvol.spc, sdvol.fat,
               sdvol.data, sdvol.root, (ticks() - t1) / 1000);
    struct fat_dir root;
    int n = 0;
    fat_root(FAT_VOL_SD, &root);
    t1 = ticks();
    int r = fat_list(&root, sd_root_entry, &n);
    log_printf("sd: root: %d entries (%d), listed in %u ms", n, r, (ticks() - t1) / 1000);
    sd_walk(&root, 0);
}
#endif
