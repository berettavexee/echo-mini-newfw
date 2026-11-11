/* The firmware's one way to change the file system (ETUDE §124): a contiguous file created at the
 * root of the internal FAT32 volume (the library's NEWFWLIB.DAT). Nothing is ever deleted or
 * moved. The writes come in an order that a power cut at any point leaves a consistent volume:
 *   1. the file's first sector (in clusters still free: nothing points at them);
 *   2. the chain in the FAT, first copy then second (cut: clusters reserved that nothing uses,
 *      which fsck reports and frees; at most the file's size);
 *   3. the directory entry, one sector written at once: the file appears whole;
 *   4. FSInfo: the free count "unknown" (the hosts count again), the next free cluster after us.
 * A root directory with no free entry first gets one more cluster (zeroed, then chained).
 * Every write goes through w_write(): only the sectors this creation planned (the FAT sectors of
 * its entries, the directory sector, FSInfo, the new clusters) pass; then emmc_write_user(), its
 * own fence set to the volume for the time of the creation. */
#include "bytes.h"
#include "emmc.h"
#include "fat.h"
#include "fatwrite.h"
#include "log.h"
#include "rtc.h"

#define SCREENRAM __attribute__((section(".screenram")))   /* buffers, not the BSS */
static uint32_t buf[128] SCREENRAM;
static struct {
    uint32_t nfats, fatsz, fsinfo, total;       /* from the boot sector */
    uint32_t lo[12], hi[12];                    /* the sectors this creation may write */
    int n;
} w;

static void allow(uint32_t lo, uint32_t n)
{
    if (w.n < 12) {
        w.lo[w.n] = lo;
        w.hi[w.n++] = lo + n;
    }
}

static int w_write(uint32_t lba, uint32_t n, const void *b)
{
    for (int i = 0; i < w.n; i++)
        if (lba >= w.lo[i] && lba + n <= w.hi[i] && lba + n > lba)
            return emmc_write_user(lba, n, b) ? FATW_EIO : 0;
    log_printf("fatwrite: sector %x refused (not planned)", lba);
    return FATW_EPROTECT;
}

static uint32_t clba(uint32_t c)
{
    return vol.data + (c - 2) * vol.spc;
}

/* FAT entry c (the first copy, one sector cached): its value, 0xffffffff on a read error */
static uint32_t fat_at = 0xffffffff, fat_buf[128] SCREENRAM;

static uint32_t entry(uint32_t c)
{
    uint32_t lba = vol.fat + c / 128;
    if (lba != fat_at) {
        fat_at = 0xffffffff;
        if (emmc_read(lba, 1, fat_buf))
            return 0xffffffff;
        fat_at = lba;
    }
    return fat_buf[c % 128] & 0x0fffffff;
}

/* A run of n free clusters, from 'from' to the end then from 2: its first, or 0 */
static uint32_t find_run(uint32_t n, uint32_t from)
{
    uint32_t last = vol.clusters + 2;
    if (from < 2 || from >= last)
        from = 2;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t c = pass ? 2 : from, end = pass ? from : last, run = 0, start = c;
        for (; c < end; c++) {
            uint32_t v = entry(c);
            if (v == 0xffffffff)
                return 0;
            if (v) {
                run = 0;
                continue;
            }
            if (!run)
                start = c;
            if (++run == n)
                return start;
        }
    }
    return 0;
}

/* FAT entries [c, c + n): each the next one, the last 'tail' (both copies, upper 4 bits kept) */
static int set_chain(uint32_t c, uint32_t n, uint32_t tail)
{
    static uint32_t s[128] SCREENRAM;
    for (uint32_t k = c; k < c + n;) {
        uint32_t lba = vol.fat + k / 128;
        if (emmc_read(lba, 1, s))
            return FATW_EIO;
        for (; k < c + n && k / 128 == (lba - vol.fat); k++)
            s[k % 128] = (s[k % 128] & 0xf0000000u) | (k + 1 < c + n ? k + 1 : tail);
        for (uint32_t f = 0; f < w.nfats; f++) {
            int r = w_write(lba + f * w.fatsz, 1, s);
            if (r)
                return r;
        }
    }
    return 0;
}

static void allow_fat(uint32_t c, uint32_t n)
{
    for (uint32_t f = 0; f < w.nfats; f++)
        allow(vol.fat + f * w.fatsz + c / 128, (c + n - 1) / 128 - c / 128 + 1);
}

/* A free entry of the root directory: its sector and index, or 0 (none in its clusters); last:
 * the root's last cluster */
static uint32_t free_slot(int *idx, uint32_t *last)
{
    static uint32_t d[128] SCREENRAM;
    for (uint32_t c = vol.root, guard = 0; c >= 2 && c < vol.clusters + 2 && guard < 4096;
         c = entry(c), guard++) {
        *last = c;
        for (uint32_t s = 0; s < vol.spc; s++) {
            if (emmc_read(clba(c) + s, 1, d))
                return 0;
            for (int i = 0; i < 16; i++) {
                uint8_t b = ((const uint8_t *)d)[32 * i];
                if (b == 0 || b == 0xe5) {
                    *idx = i;
                    return clba(c) + s;
                }
            }
        }
    }
    return 0;
}

static void name83(uint8_t *e, const char *name)
{
    int k = 0;
    for (int i = 0; i < 11; i++)
        e[i] = ' ';
    for (int i = 0; name[k] && name[k] != '.' && i < 8; i++)
        e[i] = (uint8_t)name[k++];
    while (name[k] && name[k] != '.')
        k++;
    if (name[k] == '.')
        k++;
    for (int i = 8; name[k] && i < 11; i++)
        e[i] = (uint8_t)name[k++];
}

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

int fat_create_contig(const char *name, uint32_t size, const void *first, struct fat_file *f)
{
    const uint8_t *b = (const uint8_t *)buf;
    if (!vol.mounted || vol.exfat || !size)
        return FATW_ENOVOL;
    if (!fat_find_root(name, f))
        return FATW_EXISTS;
    if (emmc_read(vol.part, 1, buf))
        return FATW_EIO;
    w.nfats = b[16];
    w.fatsz = le32(b + 36);
    w.fsinfo = vol.part + le16(b + 48);
    w.total = le32(b + 32);
    w.n = 0;
    if (w.nfats < 1 || w.nfats > 2 || le16(b + 48) < 1 || le16(b + 48) >= le16(b + 14))
        return FATW_ENOVOL;
    uint32_t hint = 2;
    if (!emmc_read(w.fsinfo, 1, buf) && le32(b) == 0x41615252 && le32(b + 484) == 0x61417272)
        hint = le32(b + 492);
    fat_at = 0xffffffff;
    uint32_t n = (size + vol.spc * 512 - 1) / (vol.spc * 512);
    int idx = 0;
    uint32_t last = 0, dir = free_slot(&idx, &last), grow = 0;
    if (!dir) {                                 /* a root without a free entry: one more */
        if (!last || !(grow = find_run(1, hint)))   /* cluster */
            return FATW_ENOSPACE;
        dir = clba(grow);
        idx = 0;
    }
    uint32_t start = find_run(n, hint);
    if (start && grow >= start && grow < start + n)
        start = find_run(n, grow + 1);          /* the run after that cluster */
    if (!start || (grow >= start && grow < start + n))
        return FATW_ENOSPACE;
    log_printf("fatwrite: %s: %u clusters at %u, entry %u of sector %x%s", name, n, start, idx,
               dir, grow ? " (the root grows)" : "");
    allow(clba(start), vol.spc);                /* the file's first cluster */
    allow_fat(start, n);
    allow(dir, 1);
    allow(w.fsinfo, 1);
    if (grow) {
        allow(clba(grow), vol.spc);
        allow_fat(grow, 1);                     /* (the root's last entry: below) */
        allow_fat(last, 1);
    }
    if (emmc_user_area(vol.part, w.total))
        return FATW_EPROTECT;
    int r = 0;
    static uint32_t s[128] SCREENRAM;
    if (grow) {                                 /* the new root cluster: zeroed, then chained */
        for (int i = 0; i < 128; i++)
            s[i] = 0;
        for (uint32_t k = 0; k < vol.spc && !r; k++)
            r = w_write(clba(grow) + k, 1, s);
        if (!r)
            r = set_chain(grow, 1, 0x0fffffff);
        if (!r)
            r = set_chain(last, 1, grow);
        if (r)
            goto out;
    }
    if ((r = w_write(clba(start), 1, first)) || (r = set_chain(start, n, 0x0fffffff)))
        goto out;
    if (emmc_read(dir, 1, s)) {
        r = FATW_EIO;
        goto out;
    }
    uint8_t *e = (uint8_t *)s + 32 * idx;
    for (int i = 0; i < 32; i++)
        e[i] = 0;
    name83(e, name);
    e[11] = 0x20;                               /* archive */
    struct rtc_tm t;
    uint32_t date = 0x5c21, tm = 0;             /* 2026-01-01 when the clock is not set */
    if (rtc_get(&t) == 1) {
        date = (uint32_t)(t.year - 1980) << 9 | t.mon << 5 | t.day;
        tm = (uint32_t)t.hour << 11 | t.min << 5 | t.sec / 2;
    }
    put16(e + 14, tm);
    put16(e + 16, date);
    put16(e + 18, date);
    put16(e + 20, start >> 16);
    put16(e + 22, tm);
    put16(e + 24, date);
    put16(e + 26, start & 0xffff);
    put32(e + 28, size);
    if ((r = w_write(dir, 1, s)))
        goto out;
    if (!emmc_read(w.fsinfo, 1, s) && s[0] == 0x41615252 && s[121] == 0x61417272) {
        s[122] = 0xffffffff;                    /* the free count: unknown */
        s[123] = start + n < vol.clusters + 2 ? start + n : 2;
        r = w_write(w.fsinfo, 1, s);
    }
    log_printf("fatwrite: %s created (%u bytes)", name, size);
out:
    emmc_user_area(0, 0);                       /* the fence closed again */
    w.n = 0;
    fat_forget();
    if (r)
        log_printf("fatwrite: %s: error %d", name, r);
    return r ? r : fat_find_root(name, f) ? FATW_EIO : 0;
}
