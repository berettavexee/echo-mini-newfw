/* Firmware installer.
 *
 * Accepted: any RKnanoFW image whose header matches its copy at fw_end and whose Rockchip
 * CRC32 over the first fw_size bytes matches the word stored right after them, with
 * fw_size = (fw_end + 0x104000) rounded down to 1 MiB (the rule of the stock and RECHO
 * images). No version or date check, so going back to any image stays possible.
 *
 * Order, so that an interruption leaves a bootable copy whenever the image fits twice:
 *   1. FW2 (only when fw_size fits in one copy), every chunk read back and compared;
 *   2. FW1 header cleared: from here the IDB loader, which boots FW1 when it is valid and FW2
 *      otherwise, starts the new FW2;
 *   3. FW1 from its second sector on, read back; 4. FW1 header last, read back.
 * Images larger than one copy (the 32 MiB stock image) only go to FW1, as RECHO does. */
#include "bytes.h"
#include "update.h"
#include "board.h"
#include "i18n.h"
#include "emmc.h"

#define CHUNK 8192

#include "shared.h"
#define buf SHARED_UPD_BUF                      /* CHUNK bytes each, shared.h */
#define chk SHARED_UPD_CHK
static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++)
            c = c & 0x80000000 ? c << 1 ^ 0x04c10db7 : c << 1;
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    while (n--)
        crc = crc << 8 ^ crc_table[(crc >> 24 ^ *p++) & 0xff];
    return crc;
}

static int same(const uint32_t *a, const uint32_t *b, uint32_t words)
{
    while (words--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

/* cause: for the log (English); title, hint: shown, translated (i18n.h) */
static const struct { int e; const char *cause, *title, *hint; } upd_msg[] = {
    { UPD_EREAD, "read error", N_("Cannot read the file"), N_("copy it to the player again") },
    { UPD_ENOTFW, "not an RKnanoFW image", N_("Not a firmware image"),
      N_("use an ECHO MINI .IMG file") },
    { UPD_ESHORT, "file shorter than its image", N_("Incomplete file"),
      N_("download or copy it again") },
    { UPD_EBIG, "image larger than the firmware area", N_("Image too large for this player"),
      N_("8 GB ECHO MINI images only") },
    { UPD_ESIZE, "size / layout not accepted", N_("Unknown image layout"),
      N_("not an image for this player") },
    { UPD_ECOPY, "header copy mismatch", N_("Damaged image"), N_("download it again") },
    { UPD_ECRC, "CRC mismatch", N_("Damaged image"), N_("download it again") },
    { UPD_EWRITE, "flash write failed", N_("Flash write failed"),
      N_("restart, then install again") },
    { UPD_EVERIFY, "flash verify failed", N_("Flash write failed"),
      N_("restart, then install again") },
};

static int upd_find(int e)
{
    for (unsigned i = 0; i < sizeof upd_msg / sizeof *upd_msg; i++)
        if (upd_msg[i].e == e)
            return (int)i;
    return -1;
}

const char *upd_error(int e)
{
    int i = upd_find(e);
    return i < 0 ? "error" : upd_msg[i].cause;
}

const char *upd_title(int e)
{
    int i = upd_find(e);
    return i < 0 ? T("Install failed") : tr(upd_msg[i].title);
}

const char *upd_hint(int e)
{
    int i = upd_find(e);
    return i < 0 ? T("restart, then try again") : tr(upd_msg[i].hint);
}

int fw_check(struct fat_file *f, struct fw_info *info, progress_fn progress)
{
    const uint8_t *b = (const uint8_t *)buf;
    uint32_t hdr[128];

    crc_init();
    fat_open(f);
    if (f->size < 0x200 || fat_read(f, hdr, 512) != 512)
        return UPD_EREAD;
    const uint8_t *h = (const uint8_t *)hdr;
    if (h[0x1f8] != 'R' || h[0x1f9] != 'K' || h[0x1fa] != 'n' || h[0x1fb] != 'a' ||
        h[0x1fc] != 'n' || h[0x1fd] != 'o' || h[0x1fe] != 'F' || h[0x1ff] != 'W')
        return UPD_ENOTFW;
    info->fw_end = hdr[0x1f4 / 4];
    info->fw_size = (info->fw_end + 0x104000) & ~0xfffffu;
    info->year = (uint16_t)le16(h);
    info->day = h[2];
    info->month = h[3];
    if (info->fw_end < 0x200 || info->fw_end % 512 || info->fw_end + 0x200 > info->fw_size)
        return UPD_ESIZE;
    if (info->fw_size / 512 > emmc.p)
        return UPD_EBIG;
    if (f->size < info->fw_size + 4)
        return UPD_ESHORT;
    info->two_copies = info->fw_size / 512 <= emmc.r;

    /* one pass: CRC over fw_size bytes, header copy compared on the way */
    uint32_t crc = 0, stored = 0;
    fat_open(f);
    for (uint32_t o = 0; o < info->fw_size + 4; o += CHUNK) {
        uint32_t n = info->fw_size + 4 - o > CHUNK ? CHUNK : info->fw_size + 4 - o;
        if (fat_read(f, buf, (n + 511) & ~511u) < (int)n)
            return UPD_EREAD;
        uint32_t c = n;
        if (o + n > info->fw_size) {            /* last chunk: the CRC word */
            c = info->fw_size - o;
            stored = b[c] | b[c + 1] << 8 | b[c + 2] << 16 | (uint32_t)b[c + 3] << 24;
        }
        crc = crc_update(crc, b, c);
        if (o <= info->fw_end && info->fw_end < o + n &&
            !same(buf + (info->fw_end - o) / 4, hdr, 128))
            return UPD_ECOPY;
        if (progress && o % (256 * 1024) == 0)
            progress(0, o, info->fw_size);
        wdt_kick();
    }
    return crc == stored ? 0 : UPD_ECRC;
}

/* Writes the image bytes [from, fw_size) at sector 'dst', each chunk read back */
static int write_copy(struct fat_file *f, const struct fw_info *info, uint32_t dst, uint32_t from,
                      int stage, progress_fn progress)
{
    fat_open(f);
    for (uint32_t o = 0; o < info->fw_size; o += CHUNK) {
        if (fat_read(f, buf, CHUNK) != CHUNK)
            return UPD_EREAD;
        if (o + CHUNK <= from)
            continue;
        uint32_t skip = o < from ? from - o : 0;
        uint32_t lba = dst + (o + skip) / 512, n = (CHUNK - skip) / 512;
        if (emmc_write_fw(lba, n, (uint8_t *)buf + skip))
            return UPD_EWRITE;
        if (emmc_read(lba, n, chk) || !same(chk, buf + skip / 4, n * 128))
            return UPD_EVERIFY;
        if (progress && o % (256 * 1024) == 0)
            progress(stage, o, info->fw_size);
    }
    return 0;
}

int fw_install(struct fat_file *f, const struct fw_info *info, progress_fn progress)
{
    int r;

    if (info->two_copies && (r = write_copy(f, info, EMMC_FW1 + emmc.r, 0, 1, progress)))
        return r;
    for (int i = 0; i < 128; i++)
        buf[i] = 0;
    if (emmc_write_fw(EMMC_FW1, 1, buf))
        return UPD_EWRITE;
    if ((r = write_copy(f, info, EMMC_FW1, 512, 2, progress)))
        return r;
    fat_open(f);
    if (fat_read(f, buf, 512) != 512)
        return UPD_EREAD;
    if (emmc_write_fw(EMMC_FW1, 1, buf))
        return UPD_EWRITE;
    if (emmc_read(EMMC_FW1, 1, chk) || !same(chk, buf, 128))
        return UPD_EVERIFY;
    if (progress)
        progress(2, info->fw_size, info->fw_size);
    return 0;
}
