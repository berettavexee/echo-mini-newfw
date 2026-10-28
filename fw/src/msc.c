/* USB mass storage, bulk only transport (USB MSC BOT 1.0) with the SCSI commands a Linux,
 * Windows or macOS host sends to a removable disk.
 * Read only mode: the medium is reported write-protected and writes are refused with DATA
 * PROTECT, their data read and dropped. Read/write mode: WRITE (10) goes to the eMMC through
 * emmc_write_user(), fenced to the exposed sectors; every block is on the card before the
 * CSW, so there is no cache to flush.
 *
 * When the device has less data than the host asked for, it sends what it has; if that ends
 * on a full packet it pads with zeros so the host's transfer ends, and the CSW residue tells
 * the host how much is valid. */
#include "msc.h"
#ifdef ECHO_PROBE
#include "probe.h"
#endif
#include "board.h"
#include "bytes.h"
#include "emmc.h"
#include "usb.h"
#include "version.h"

#define CHUNK_SECTORS 128               /* 64 KiB: eMMC runs and USB batches */

struct msc_state msc;

static uint32_t cbw_buf[16];
#include "shared.h"
#define data SHARED_USB                         /* CHUNK_SECTORS * 512 bytes, shared.h */
static int armed;
static uint8_t sense_key, sense_asc;

static void sense(uint8_t key, uint8_t asc)
{
    sense_key = key;
    sense_asc = asc;
}

/* Zeros until 'left' bytes have gone, in packets the host can take */
static int pad(uint32_t left)
{
    uint8_t *z = (uint8_t *)data;
    for (uint32_t i = 0; i < 512; i++)
        z[i] = 0;
    while (left) {
        uint32_t n = left > 512 ? 512 : left;
        int r = usb_in(z, n);
        if (r)
            return r;
        left -= n;
    }
    return 0;
}

/* Data-in phase of a short reply: returns the residue */
static int reply(const void *buf, uint32_t n, uint32_t expected, uint32_t *residue)
{
    if (n > expected)
        n = expected;
    *residue = expected - n;
    if (!expected)
        return 0;
    int r = usb_in(buf, n);
    if (!r && n < expected && n % usb.mps == 0)
        r = pad(expected - n);
    return r;
}

/* Data-out phase the device does not want: read and drop it */
static int drop(uint32_t left)
{
    while (left) {
        uint32_t n = left > 3 * usb.mps ? 3 * usb.mps : left, got;   /* one transfer */
        int r = usb_out(data, n, &got);
        if (r)
            return r;
        if (!got)
            break;
        left -= got < n ? got : n;
        if (got < n)
            break;
    }
    return 0;
}

static int write10(const uint8_t *cb, uint32_t expected, uint32_t *residue)
{
    uint32_t lba = be32(cb + 2), n = (uint32_t)cb[7] << 8 | cb[8], done = 0;

    *residue = expected;
    if (lba >= msc.count || n > msc.count - lba || n * 512 != expected) {
        sense(5, lba >= msc.count || n > msc.count - lba ? 0x21 : 0x24);
        return drop(expected) ? -1 : 1;
    }
    while (n) {
        uint32_t k = n > CHUNK_SECTORS ? CHUNK_SECTORS : n, have = 0;
        uint8_t *d = (uint8_t *)data;
        while (have < k * 512) {
            uint32_t want = k * 512 - have, got;
            if (want > 3 * usb.mps)
                want = 3 * usb.mps;
            if (usb_out(d + have, want, &got) || !got)
                return -1;
            have += got;
        }
        done += have;
#ifdef ECHO_PROBE
        if (probe_hit(lba, k)) {                /* the USB probe's mailbox: not the eMMC */
            probe_write(lba, k, data);
            msc.written_sectors += k;
            lba += k;
            n -= k;
            continue;
        }
#endif
        if (emmc_write_user(msc.base + lba, k, data)) {
            msc.errors++;
            sense(3, 0x0c);                             /* write error */
            *residue = expected - done;
            return drop(expected - done) ? -1 : 1;
        }
        msc.written_sectors += k;
        lba += k;
        n -= k;
    }
    *residue = 0;
    return 0;
}

static int read10(const uint8_t *cb, uint32_t expected, uint32_t *residue)
{
    uint32_t lba = be32(cb + 2), n = (uint32_t)cb[7] << 8 | cb[8], sent = 0;

    *residue = expected;
    if (lba >= msc.count || n > msc.count - lba) {
        sense(5, 0x21);                                 /* LBA out of range */
        return pad(expected) ? -1 : 1;
    }
    if (n * 512 > expected)
        n = expected / 512;
    while (n) {
        uint32_t k = n > CHUNK_SECTORS ? CHUNK_SECTORS : n;
        if (emmc_read(msc.base + lba, k, data)) {
            msc.errors++;
            sense(3, 0x11);                             /* unrecovered read error */
            return pad(expected - sent) ? -1 : 1;
        }
#ifdef ECHO_PROBE
        if (probe_hit(lba, k))
            probe_read(lba, k, data);           /* the USB probe's answer */
#endif
        if (usb_in(data, k * 512))
            return -1;
        msc.read_sectors += k;
        sent += k * 512;
        lba += k;
        n -= k;
    }
    *residue = expected - sent;
    if (*residue && pad(*residue))
        return -1;
    return 0;
}

/* Runs one command: returns the CSW status (0 passed, 1 failed), or -1 when the transport was
 * interrupted (bus reset, mass storage reset, cable) and no CSW must be sent */
static int command(const uint8_t *cb, uint32_t expected, int dir_in, uint32_t *residue)
{
    uint8_t r[36];
    int e;

    *residue = expected;
    msc.last_op = cb[0];
    switch (cb[0]) {
    case 0x00:                                          /* TEST UNIT READY */
        if (msc.ejected) {
            sense(2, 0x3a);                             /* medium not present */
            return 1;
        }
        return 0;
    case 0x03: {                                        /* REQUEST SENSE */
        uint8_t s[18] = { 0x70, 0, sense_key, 0, 0, 0, 0, 10, 0, 0, 0, 0, sense_asc, 0 };
        sense(0, 0);
        return reply(s, sizeof s, expected, residue) ? -1 : 0;
    }
    case 0x12: {                                        /* INQUIRY */
        static const char id[] = "newfw   ECHO MINI       " FW_VERSION;
        if (cb[1] & 1) {                                /* vital product data: not supported */
            sense(5, 0x24);
            return expected && pad(expected) ? -1 : 1;
        }
        for (int i = 0; i < 36; i++)
            r[i] = 0;
        r[1] = 0x80;                                    /* removable */
        r[2] = 4;                                       /* SPC-2 */
        r[3] = 2;
        r[4] = 31;
        for (int i = 0; i < 28 && id[i]; i++)
            r[8 + i] = (uint8_t)id[i];
        for (int i = 32; i < 36; i++)
            if (!r[i])
                r[i] = ' ';
        return reply(r, 36, expected, residue) ? -1 : 0;
    }
    case 0x1a:                                          /* MODE SENSE (6) */
        r[0] = 3; r[1] = 0; r[2] = msc.writable ? 0 : 0x80; r[3] = 0;   /* write protect */
        return reply(r, 4, expected, residue) ? -1 : 0;
    case 0x5a:                                          /* MODE SENSE (10) */
        for (int i = 0; i < 8; i++)
            r[i] = 0;
        r[1] = 6;
        r[3] = msc.writable ? 0 : 0x80;
        return reply(r, 8, expected, residue) ? -1 : 0;
    case 0x1b:                                          /* START STOP UNIT */
        if ((cb[4] & 3) == 2)
            msc.ejected = 1;
        return 0;
    case 0x1e:                                          /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2f:                                          /* VERIFY (10) */
    case 0x35:                                          /* SYNCHRONIZE CACHE (10) */
        return 0;
    case 0x23:                                          /* READ FORMAT CAPACITIES */
        for (int i = 0; i < 12; i++)
            r[i] = 0;
        r[3] = 8;
        put_be32(r + 4, msc.count);
        r[8] = 2;                                       /* formatted medium */
        r[10] = 2;                                      /* 512-byte blocks */
        return reply(r, 12, expected, residue) ? -1 : 0;
    case 0x25:                                          /* READ CAPACITY (10) */
        put_be32(r, msc.count - 1);
        put_be32(r + 4, 512);
        return reply(r, 8, expected, residue) ? -1 : 0;
    case 0x28:                                          /* READ (10) */
        e = read10(cb, expected, residue);
        return e;
    case 0x2a:                                          /* WRITE (10) */
        if (msc.writable)
            return write10(cb, expected, residue);
        /* fall through */
    case 0x0a:                                          /* WRITE (6) */
        sense(7, 0x27);                                 /* write protected */
        if (!dir_in && drop(expected))
            return -1;
        return 1;
    default:
        sense(5, 0x20);                                 /* invalid command operation code */
        if (expected && (dir_in ? pad(expected) : drop(expected)))
            return -1;
        return 1;
    }
}

int msc_start(uint32_t base, uint32_t count, int writable)
{
    msc.base = base;
    msc.count = count;
    msc.writable = 0;
    if (writable) {
        if (emmc_user_area(base, count))
            return -1;
        msc.writable = 1;
    }
    msc.commands = msc.read_sectors = msc.written_sectors = msc.errors = 0;
    msc.last_op = 0;
    msc.ejected = 0;
    armed = 0;
    sense(0, 0);
    return 0;
}

void msc_poll(void)
{
    uint32_t got, residue;
    const uint8_t *c = (const uint8_t *)cbw_buf;

    usb_service();
    if (usb.bot_reset) {
        usb.bot_reset = 0;
        armed = 0;
    }
    if (!usb.configured) {
        armed = 0;
        return;
    }
    if (!armed) {
        usb_out_arm(cbw_buf, 31);
        armed = 1;
        return;
    }
    if (!usb_out_done(&got))
        return;
    armed = 0;
    if (got != 31 || be32(c) != 0x55534243) {           /* "USBC" read big-endian */
        msc.errors++;
        return;
    }
    msc.commands++;
    uint32_t expected = c[8] | c[9] << 8 | c[10] << 16 | (uint32_t)c[11] << 24;
    int status = command(c + 15, expected, c[12] & 0x80, &residue);
    if (status < 0)
        return;
    uint8_t csw[13] = { 'U', 'S', 'B', 'S', c[4], c[5], c[6], c[7],
                        (uint8_t)residue, (uint8_t)(residue >> 8), (uint8_t)(residue >> 16),
                        (uint8_t)(residue >> 24), (uint8_t)status };
    if (usb_in(csw, sizeof csw))
        msc.errors++;
}
