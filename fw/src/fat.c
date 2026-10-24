/* Read-only FAT32 and exFAT: volume search, directory listings, file reads. The internal volume
 * (eMMC) is FAT32; the microSD card may be FAT32 or exFAT (ETUDE §37). exFAT follows Microsoft's
 * public specification (2019): boot sector and its checksum, FAT of 32-bit entries, directory
 * entry sets (file 0x85, stream extension 0xc0, file name 0xc1), "no FAT chain" files. */
#include "fat.h"
#include "emmc.h"
#include "sd.h"
#include "text.h"

struct fat_vol vol, sdvol;

static uint32_t sec[128];       /* one sector */
static uint32_t fat_sec[2][128], fat_cached[2] = { 0xffffffff, 0xffffffff };  /* per volume */

static struct fat_vol *V(int v)
{
    return v == FAT_VOL_SD ? &sdvol : &vol;
}

static int dev_read(const struct fat_vol *v, uint32_t lba, uint32_t n, void *buf)
{
    return v->dev == FAT_VOL_SD ? sd_read(lba, n, buf) : emmc_read(lba, n, buf);
}

/* A FAT32 boot sector at 'lba'? Fills v. */
static int boot_sector(struct fat_vol *v, uint32_t lba)
{
    const uint8_t *b = (const uint8_t *)sec;

    if (dev_read(v, lba, 1, sec) || b[510] != 0x55 || b[511] != 0xaa)
        return 0;
    if (le16(b + 11) != 512 || !b[13] || (b[13] & (b[13] - 1)) || !b[16] || le16(b + 17) != 0)
        return 0;
    uint32_t fatsz = le32(b + 36), total = le32(b + 32);
    if (!fatsz || !total || le16(b + 22) != 0)
        return 0;
    v->part = lba;
    v->spc = b[13];
    v->fat = lba + le16(b + 14);
    v->data = v->fat + b[16] * fatsz;
    v->clusters = (total - (v->data - lba)) / v->spc;
    v->root = le32(b + 44);
    v->exfat = 0;
    return v->clusters >= 65525 && v->root >= 2;
}

/* An exFAT boot region at 'lba'? Its checksum (sectors 0-10, sector 11 repeats it) checked */
static int exfat_boot(struct fat_vol *v, uint32_t lba)
{
    const uint8_t *b = (const uint8_t *)sec;

    if (dev_read(v, lba, 1, sec) || b[510] != 0x55 || b[511] != 0xaa ||
        __builtin_memcmp(b + 3, "EXFAT   ", 8) || b[108] != 9 || b[109] > 16 || !b[110])
        return 0;
    uint32_t fat_off = le32(b + 80), fat_len = le32(b + 84), heap = le32(b + 88),
             count = le32(b + 92), root = le32(b + 96), shift = b[109], second = b[106] & 1;
    uint32_t sum = 0;
    for (uint32_t s = 0; s < 11; s++) {
        if (s && dev_read(v, lba + s, 1, sec))
            return 0;
        for (int i = 0; i < 512; i++)
            if (s || (i != 106 && i != 107 && i != 112))
                sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + b[i];
    }
    if (dev_read(v, lba + 11, 1, sec) || sec[0] != sum || sec[127] != sum)
        return 0;
    if (!count || root < 2 || root >= count + 2 || !fat_off || !heap)
        return 0;
    v->part = lba;
    v->spc = 1u << shift;
    v->fat = lba + fat_off + (second && b[110] > 1 ? fat_len : 0);   /* the active FAT */
    v->data = lba + heap;
    v->clusters = count;
    v->root = root;
    v->exfat = 1;
    return 1;
}

static int volume_at(uint32_t lba)
{
    const uint8_t *b = (const uint8_t *)sec;

    if (boot_sector(&vol, lba))
        return 1;
    if (emmc_read(lba, 1, sec) || b[510] != 0x55 || b[511] != 0xaa)
        return 0;
    for (int i = 0; i < 4; i++) {               /* MBR */
        const uint8_t *e = b + 446 + 16 * i;
        if ((e[4] == 0x0b || e[4] == 0x0c) && le32(e + 8)) {
            uint32_t start = lba + le32(e + 8);
            if (boot_sector(&vol, start))
                return 1;
            emmc_read(lba, 1, sec);
        }
    }
    return 0;
}

int fat_mount(uint32_t from)
{
    fat_cached[FAT_VOL_INT] = 0xffffffff;      /* the host may have changed the volume */
    vol.dev = FAT_VOL_INT;
    vol.mounted = 0;
    for (uint32_t lba = from; lba < from + 0x100000; lba += 0x800) {
        if (volume_at(lba)) {
            vol.base = lba;
            vol.mounted = 1;
            return 0;
        }
    }
    return -1;
}

/* The card: a volume at sector 0 (no partition table), else the first FAT32 (0x0b, 0x0c) or
 * exFAT (0x07) partition of the MBR */
int fat_mount_sd(void)
{
    const uint8_t *b = (const uint8_t *)sec;
    fat_cached[FAT_VOL_SD] = 0xffffffff;
    sdvol.dev = FAT_VOL_SD;
    sdvol.mounted = 0;
    sdvol.base = 0;
    if (exfat_boot(&sdvol, 0) || boot_sector(&sdvol, 0)) {
        sdvol.mounted = 1;
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        if (dev_read(&sdvol, 0, 1, sec) || b[510] != 0x55 || b[511] != 0xaa)
            return -1;
        const uint8_t *e = b + 446 + 16 * i;
        uint32_t start = le32(e + 8);
        if (!start)
            continue;
        if ((e[4] == 0x07 && exfat_boot(&sdvol, start)) ||
            ((e[4] == 0x0b || e[4] == 0x0c) && boot_sector(&sdvol, start))) {
            sdvol.mounted = 1;
            return 0;
        }
    }
    return -1;
}

/* The cluster after c; anything that is not a cluster (end of chain, bad) is above the range */
static uint32_t next_cluster(const struct fat_vol *v, uint32_t c)
{
    int i = v->dev == FAT_VOL_SD;
    uint32_t lba = v->fat + c / 128;

    if (lba != fat_cached[i]) {
        fat_cached[i] = 0xffffffff;
        if (dev_read(v, lba, 1, fat_sec[i]))
            return 0xffffffff;
        fat_cached[i] = lba;
    }
    uint32_t n = fat_sec[i][c % 128];
    return v->exfat ? n : n & 0x0fffffff;
}

static int valid(const struct fat_vol *v, uint32_t c)
{
    return c >= 2 && c < v->clusters + 2;
}

static uint32_t cluster_lba(const struct fat_vol *v, uint32_t c)
{
    return v->data + (c - 2) * v->spc;
}

/* The next cluster of a file or directory: exFAT "no FAT chain" ones are contiguous */
static uint32_t step(const struct fat_vol *v, int contig, uint32_t c)
{
    return contig ? c + 1 : next_cluster(v, c);
}

int fat_list_root(int (*fn)(const struct fat_file *f, void *ctx), void *ctx)
{
    static uint32_t dir[128];
    struct fat_file f;

    for (uint32_t c = vol.root, guard = 0; valid(&vol, c) && guard < 4096;
         c = next_cluster(&vol, c), guard++) {
        for (uint32_t s = 0; s < vol.spc; s++) {
            if (emmc_read(cluster_lba(&vol, c) + s, 1, dir))
                return -1;
            for (int i = 0; i < 16; i++) {
                const uint8_t *e = (const uint8_t *)dir + 32 * i;
                if (e[0] == 0)
                    return 0;
                if (e[0] == 0xe5 || e[11] == 0x0f || (e[11] & 0x18))
                    continue;                   /* deleted, long name, volume label, directory */
                int n = 0;
                for (int k = 0; k < 8 && e[k] != ' '; k++)
                    f.name[n++] = e[k];
                if (e[8] != ' ') {
                    f.name[n++] = '.';
                    for (int k = 8; k < 11 && e[k] != ' '; k++)
                        f.name[n++] = e[k];
                }
                f.name[n] = 0;
                f.cluster = le16(e + 20) << 16 | le16(e + 26);
                f.size = le32(e + 28);
                f.vol = FAT_VOL_INT;
                f.contig = 0;
                if (fn(&f, ctx))
                    return 0;
            }
        }
    }
    return 0;
}

void fat_open(struct fat_file *f)
{
    f->pos = 0;
    f->cur = f->cluster;
}

int fat_read(struct fat_file *f, void *buf, uint32_t n)
{
    const struct fat_vol *v = V(f->vol);
    uint32_t done = 0, csize = v->spc * 512;

    if (n > f->size - f->pos)
        n = (f->size - f->pos + 511) & ~511u;
    while (done < n) {
        if (!valid(v, f->cur))
            return -1;
        uint32_t off = f->pos % csize, k = csize - off;
        if (k > n - done)
            k = n - done;
        if (dev_read(v, cluster_lba(v, f->cur) + off / 512, k / 512, (uint8_t *)buf + done))
            return -1;
        done += k;
        f->pos += k;
        if (f->pos % csize == 0)
            f->cur = step(v, f->contig, f->cur);
    }
    return done;
}

/* "Not properly unmounted" marks of a FAT32 volume, as a bit mask (0 = clean):
 *  FAT_DIRTY_BS: bit 0 of the boot sector's state byte (+0x41), the one Linux sets while the
 *                volume is mounted read/write and reports at mount ("Volume was not properly
 *                unmounted"); it only clears it at unmount when it found it clear, so once
 *                set it stays until fsck.fat repairs it;
 *  FAT_DIRTY_FAT: clean-shutdown bit (27) of FAT entry 1 at 0, used by Windows;
 *  FAT_DIRTY_ERR: hard-error bit (26) of FAT entry 1 at 0. */
int fat_dirty(void)
{
    const uint8_t *b = (const uint8_t *)sec;
    int d = 0;

    if (emmc_read(vol.part, 1, sec))
        return -1;
    if (b[0x41] & 1)
        d |= FAT_DIRTY_BS;
    if (emmc_read(vol.fat, 1, sec))
        return -1;
    if (!(sec[1] & 1u << 27))
        d |= FAT_DIRTY_FAT;
    if (!(sec[1] & 1u << 26))
        d |= FAT_DIRTY_ERR;
    return d;
}

/* Data sectors of a file if its clusters are contiguous (first sector, count); a fragmented
 * or empty file returns -1 */
int fat_extent(const struct fat_file *f, uint32_t *first, uint32_t *count)
{
    const struct fat_vol *v = V(f->vol);
    uint32_t clusters = (f->size + v->spc * 512 - 1) / (v->spc * 512), c = f->cluster;

    if (!clusters || !valid(v, c))
        return -1;
    for (uint32_t i = 1; i < clusters; i++) {
        uint32_t n = step(v, f->contig, c);
        if (n != c + 1)
            return -1;
        c = n;
    }
    *first = cluster_lba(v, f->cluster);
    *count = clusters * v->spc;
    return 0;
}

static int find_cb(const struct fat_file *f, void *ctx)
{
    struct fat_file *want = ctx;
    for (int i = 0; ; i++) {
        if (f->name[i] != want->name[i])
            return 0;
        if (!f->name[i])
            break;
    }
    *want = *f;
    want->pos = 1;                              /* found */
    return 1;
}

int fat_find_root(const char *name, struct fat_file *f)
{
    int i = 0;
    for (; name[i] && i < 12; i++)
        f->name[i] = name[i];
    f->name[i] = 0;
    f->pos = 0;
    if (fat_list_root(find_cb, f) || !f->pos)
        return -1;
    f->pos = 0;
    return 0;
}

int fat_pread(const struct fat_file *f, struct fat_cursor *c, uint32_t pos, void *buf, uint32_t n)
{
    static uint32_t tmp[128];
    const struct fat_vol *v = V(f->vol);
    uint8_t *d = buf;
    uint32_t csize = v->spc * 512, done = 0;

    if (pos > f->size || n > f->size - pos)
        return -1;
    if (!c->cluster || pos / csize < c->index) {
        c->index = 0;
        c->cluster = f->cluster;
    }
    while (done < n) {
        uint32_t p = pos + done, want = p / csize;
        while (c->index < want) {
            c->cluster = step(v, f->contig, c->cluster);
            c->index++;
            if (!valid(v, c->cluster))
                return -1;
        }
        uint32_t lba = cluster_lba(v, c->cluster) + p % csize / 512, off = p % 512;
        uint32_t k = n - done;
        if (!off && k >= 512 && ((uint32_t)(d + done) & 3) == 0) {
            /* whole sectors straight into the buffer, over the following clusters too while they
             * are contiguous: one command for the run (each costs a fixed delay) */
            uint32_t secs = k / 512, left = (csize - p % csize) / 512;
            while (left < secs) {
                uint32_t nx = step(v, f->contig, c->cluster);
                if (nx != c->cluster + 1 || !valid(v, nx))
                    break;
                c->cluster = nx;
                c->index++;
                left += v->spc;
            }
            if (secs > left)
                secs = left;
            if (dev_read(v, lba, secs, d + done))
                return -1;
            done += secs * 512;
            continue;
        }
        if (dev_read(v, lba, 1, tmp))
            return -1;
        if (k > 512 - off)
            k = 512 - off;
        for (uint32_t i = 0; i < k; i++)
            d[done + i] = ((uint8_t *)tmp)[off + i];
        done += k;
    }
    return (int)n;
}

/* ---- directories with long names ---- */
static uint8_t lfn_sum(const uint8_t *e)
{
    uint8_t s = 0;
    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

/* UTF-16 code units to UTF-8, at most max - 1 bytes, whole characters only */
static void utf16_to_utf8(const uint16_t *u, int n, char *out, int max)
{
    int o = 0;
    for (int i = 0; i < n && u[i]; i++) {
        uint32_t c = u[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && u[i + 1] >= 0xdc00 && u[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (u[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000)
            c = 0xfffd;
        if (!utf8_put(out, max, &o, c))
            break;
    }
    out[o] = 0;
}

static uint32_t dir[128];
static uint16_t lfn[20 * 13 + 1];
static struct fat_dirent d;

static int fat32_list(const struct fat_vol *v, int vi, uint32_t cluster,
                      int (*fn)(const struct fat_dirent *e, void *ctx), void *ctx)
{
    static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    int lfn_seq = 0, lfn_len = 0;
    uint8_t lfn_ck = 0;

    for (uint32_t c = cluster, guard = 0; valid(v, c) && guard < 65536;
         c = next_cluster(v, c), guard++) {
        for (uint32_t s = 0; s < v->spc; s++) {
            if (dev_read(v, cluster_lba(v, c) + s, 1, dir))
                return -1;
            for (int i = 0; i < 16; i++) {
                const uint8_t *e = (const uint8_t *)dir + 32 * i;
                if (e[0] == 0)
                    return 0;
                if (e[0] == 0xe5) {
                    lfn_seq = 0;
                    continue;
                }
                if (e[11] == 0x0f) {            /* a piece of a long name, last piece first */
                    int seq = e[0] & 0x1f;
                    if (e[0] & 0x40) {
                        if (seq < 1 || seq > 20) {
                            lfn_seq = 0;
                            continue;
                        }
                        lfn_len = seq * 13;
                        lfn_ck = e[13];
                        for (int k = 0; k <= lfn_len; k++)
                            lfn[k] = 0;
                    } else if (seq != lfn_seq - 1 || e[13] != lfn_ck) {
                        lfn_seq = 0;
                        continue;
                    }
                    lfn_seq = seq;
                    for (int k = 0; k < 13; k++) {
                        uint16_t u = le16(e + pos[k]);
                        lfn[(seq - 1) * 13 + k] = u == 0xffff ? 0 : u;
                    }
                    continue;
                }
                int have_lfn = lfn_seq == 1 && lfn_sum(e) == lfn_ck;
                lfn_seq = 0;
                if ((e[11] & 0x08) || (e[11] & 0x06) || e[0] == '.')
                    continue;                   /* volume label, hidden, system, '.', '..' */
                int n = 0;
                for (int k = 0; k < 8 && e[k] != ' '; k++)
                    d.sname[n++] = (char)(k == 0 && e[k] == 0x05 ? 0xe5 : e[k]);
                if (e[8] != ' ') {
                    d.sname[n++] = '.';
                    for (int k = 8; k < 11 && e[k] != ' '; k++)
                        d.sname[n++] = (char)e[k];
                }
                d.sname[n] = 0;
                if (have_lfn)
                    utf16_to_utf8(lfn, lfn_len, d.name, FAT_NAME_MAX);
                else {
                    /* 8.3 only: lower case flags of Windows NT (0x08 base, 0x10 extension) */
                    int dot = 0;
                    for (int k = 0; k <= n; k++) {
                        char ch = d.sname[k];
                        if (ch == '.')
                            dot = 1;
                        if (ch >= 'A' && ch <= 'Z' && (e[12] & (dot ? 0x10 : 0x08)))
                            ch = (char)(ch + 32);
                        d.name[k] = ch;
                    }
                }
                if (d.name[0] == '.')
                    continue;                   /* .Trash-1000 and the like */
                d.attr = e[11];
                d.vol = (uint8_t)vi;
                d.contig = 0;
                d.cluster = (uint32_t)le16(e + 20) << 16 | le16(e + 26);
                d.size = le32(e + 28);
                d.mtime = (uint32_t)le16(e + 24) << 16 | le16(e + 22);
                if (fn(&d, ctx))
                    return 0;
            }
        }
    }
    return 0;
}

/* exFAT: an entry set is a file entry (0x85: attributes, times, the count of the entries that
 * follow), a stream extension (0xc0: flags, name length, first cluster, sizes) and file name
 * entries (0xc1: 15 UTF-16 units each). A type below 0x80 is unused; 0 ends the directory.
 * Sets may cross sectors and clusters: they are followed entry by entry. */
static int exfat_list(const struct fat_vol *v, int vi, uint32_t cluster, int contig, uint32_t size,
                      int (*fn)(const struct fat_dirent *e, void *ctx), void *ctx)
{
    uint32_t left = 0, got = 0, name_len = 0, attr = 0, mtime = 0, first = 0, len_lo = 0,
             big = 0, flags = 0, stream = 0, bytes = 0;

    for (uint32_t c = cluster, guard = 0; valid(v, c) && guard < 65536;
         c = step(v, contig, c), guard++) {
        for (uint32_t s = 0; s < v->spc; s++, bytes += 512) {
            if (size && bytes >= size)
                return 0;
            if (dev_read(v, cluster_lba(v, c) + s, 1, dir))
                return -1;
            for (int i = 0; i < 16; i++) {
                const uint8_t *e = (const uint8_t *)dir + 32 * i;
                if (e[0] == 0)
                    return 0;
                if (!(e[0] & 0x80)) {           /* deleted */
                    left = 0;
                    continue;
                }
                if (e[0] == 0x85) {
                    left = e[1];
                    got = name_len = stream = 0;
                    attr = le16(e + 4);
                    mtime = le32(e + 12);
                    if (left < 2 || left > 18)
                        left = 0;
                    continue;
                }
                if (!left)
                    continue;                   /* bitmap, up-case table, label, stray entries */
                left--;
                if (e[0] == 0xc0 && !stream) {
                    stream = 1;
                    flags = e[1];
                    name_len = e[3];
                    first = le32(e + 20);
                    /* files: the valid data length, beyond which nothing was written */
                    uint32_t lo = le32(attr & FAT_ATTR_DIR ? e + 24 : e + 8);
                    big = le32(attr & FAT_ATTR_DIR ? e + 28 : e + 12) != 0;
                    len_lo = lo;
                } else if (e[0] == 0xc1 && stream) {
                    for (int k = 0; k < 15 && got < name_len; k++)
                        lfn[got++] = le16(e + 2 + 2 * k);
                }
                if (left || !stream || got != name_len || !name_len)
                    continue;
                if ((attr & 0x06) || big)
                    continue;                   /* hidden, system, 4 GiB or more */
                lfn[got] = 0;
                utf16_to_utf8(lfn, (int)got, d.name, FAT_NAME_MAX);
                if (d.name[0] == '.')
                    continue;
                int n = 0;                      /* no 8.3 name: the start of the long one, */
                while (n < 12 && d.name[n])     /* cut on a character boundary */
                    n++;
                while (n && ((uint8_t)d.name[n] & 0xc0) == 0x80)
                    n--;
                for (int k = 0; k < n; k++)
                    d.sname[k] = d.name[k];
                d.sname[n] = 0;
                d.attr = (uint8_t)attr;
                d.vol = (uint8_t)vi;
                d.contig = (flags & 2) != 0;
                d.cluster = first;
                d.size = len_lo;
                d.mtime = mtime;
                if (fn(&d, ctx))
                    return 0;
            }
        }
    }
    return 0;
}

int fat_list(const struct fat_dir *dr, int (*fn)(const struct fat_dirent *e, void *ctx), void *ctx)
{
    const struct fat_vol *v = V(dr->vol);
    if (v == &sdvol && !sdvol.mounted)
        return -1;
    if (!dr->cluster)                           /* the root: a FAT chain in both */
        return v->exfat ? exfat_list(v, dr->vol, v->root, 0, 0, fn, ctx) :
                          fat32_list(v, dr->vol, v->root, fn, ctx);
    return v->exfat ? exfat_list(v, dr->vol, dr->cluster, dr->contig, dr->size, fn, ctx) :
                      fat32_list(v, dr->vol, dr->cluster, fn, ctx);
}

int fat_list_dir(uint32_t cluster, int (*fn)(const struct fat_dirent *e, void *ctx), void *ctx)
{
    struct fat_dir dr = { cluster, 0, FAT_VOL_INT, 0 };
    return fat_list(&dr, fn, ctx);
}

void fat_dir_of(const struct fat_dirent *e, struct fat_dir *dr)
{
    dr->cluster = e->cluster;
    dr->size = e->size;
    dr->vol = e->vol;
    dr->contig = e->contig;
}

void fat_root(int v, struct fat_dir *dr)
{
    dr->cluster = dr->size = 0;
    dr->vol = (uint8_t)v;
    dr->contig = 0;
}

void fat_file_of(const struct fat_dirent *e, struct fat_file *f)
{
    for (int i = 0; i < 13; i++)
        f->name[i] = e->sname[i];
    f->vol = e->vol;
    f->contig = e->contig;
    f->cluster = e->cluster;
    f->size = e->size;
    fat_open(f);
}
