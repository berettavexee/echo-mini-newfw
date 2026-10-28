/* Host-side test of src/msc.c: the bulk-only transport and the SCSI commands, with a fake USB
 * layer playing the host and a fake eMMC backed by memory.
 *
 *   cc -O1 -Wall -Isrc -o build/msc_test tools/msc_test.c && build/msc_test
 *
 * The fake eMMC applies the same fence rules as src/emmc.c (writes only inside the declared
 * user area, which must start after the firmware area) and counts every write; the tests check
 * the data, the CSW status and residue, and that nothing outside the area changes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- fakes for board.h, emmc.h, usb.h ---- */
#define BOARD_H
#define EMMC_H
#define USB_H
void wdt_kick(void) {}
uint32_t ticks(void) { static uint32_t t; return t += 10; }

#define EMMC_FW1 0x2000u
#define FW_AREA_END (EMMC_FW1 + 0x1b000u)
#define DISK_SECTORS 0x90000u
static uint8_t *disk;
static uint32_t user_lo, user_hi, writes_outside, writes;
int emmc_read(uint32_t s, uint32_t n, void *buf)
{
    if (s + n > DISK_SECTORS)
        return -4;
    memcpy(buf, disk + 512ull * s, 512ull * n);
    return 0;
}
int emmc_user_area(uint32_t base, uint32_t count)
{
    user_lo = user_hi = 0;
    if (base < FW_AREA_END || base + count < base || !count)
        return -6;
    user_lo = base;
    user_hi = base + count;
    return 0;
}
int emmc_write_user(uint32_t s, uint32_t n, const void *buf)
{
    if (user_hi <= user_lo || s < user_lo || s + n > user_hi || s + n < s)
        return -6;
    if (s < 0x81000)
        writes_outside++;
    writes += n;
    memcpy(disk + 512ull * s, buf, 512ull * n);
    return 0;
}

struct usb_state {
    int configured, high_speed;
    uint32_t mps, resets, setups;
    int bot_reset;
};
struct usb_state usb = { 1, 1, 512, 0, 0, 0 };
enum { USB_EABORT = -20, USB_ETIMEOUT = -21 };

/* host OUT transfers, in order; device IN bytes, captured */
static struct { uint8_t *p; uint32_t len, pos; } outq[8];
static int outn;
static uint8_t inbuf[1 << 22];
static uint32_t inlen;
static uint8_t *arm_buf;
static uint32_t arm_len;

void usb_service(void) {}
void usb_out_arm(void *buf, uint32_t len)
{
    if (len > 3 * usb.mps)
        len = 3 * usb.mps;
    arm_buf = buf;
    arm_len = len;
}
int usb_out_done(uint32_t *got)
{
    if (!outn)
        return 0;
    uint32_t n = outq[0].len - outq[0].pos;
    if (n > arm_len)
        n = arm_len;
    memcpy(arm_buf, outq[0].p + outq[0].pos, n);
    outq[0].pos += n;
    if (outq[0].pos == outq[0].len) {
        free(outq[0].p);
        memmove(outq, outq + 1, sizeof outq[0] * --outn);
    }
    *got = n;
    return 1;
}
int usb_out(void *buf, uint32_t len, uint32_t *got)
{
    usb_out_arm(buf, len);
    if (!usb_out_done(got))
        return USB_ETIMEOUT;
    return 0;
}
int usb_in(const void *buf, uint32_t len)
{
    memcpy(inbuf + inlen, buf, len);
    inlen += len;
    return 0;
}

/* The device's shared 64 KiB buffer at a fixed address (shared.h): host memory here */
#define SHARED_H
static uint32_t shared_usb[16384];
#define SHARED_USB shared_usb

#include "../src/msc.c"

/* ---- host side ---- */
static void push(const void *p, uint32_t len)
{
    outq[outn].p = malloc(len);
    memcpy(outq[outn].p, p, len);
    outq[outn].len = len;
    outq[outn++].pos = 0;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                      printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Sends one command; returns the CSW status, the data-in bytes in *data (inbuf), residue */
static int run(const uint8_t *cb, int cblen, uint32_t len, int in, const void *out,
               uint32_t *residue)
{
    static uint32_t tag = 0x1000;
    uint8_t cbw[31] = { 'U', 'S', 'B', 'C' };
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = (uint8_t)cblen;
    memcpy(cbw + 15, cb, cblen);
    inlen = 0;
    push(cbw, 31);
    if (!in && len && out)
        push(out, len);
    for (int i = 0; i < 4 && inlen < 13; i++)
        msc_poll();
    if (inlen < 13) {
        printf("no CSW\n");
        failures++;
        return -1;
    }
    const uint8_t *csw = inbuf + inlen - 13;
    CHECK(!memcmp(csw, "USBS", 4), "CSW signature");
    CHECK(!memcmp(csw + 4, &tag, 4), "CSW tag");
    memcpy(residue, csw + 8, 4);
    inlen -= 13;
    tag++;
    return csw[12];
}

static void rw10(uint8_t *cb, uint8_t op, uint32_t lba, uint16_t n)
{
    memset(cb, 0, 10);
    cb[0] = op;
    cb[2] = lba >> 24; cb[3] = lba >> 16; cb[4] = lba >> 8; cb[5] = lba;
    cb[7] = n >> 8; cb[8] = n;
}

int main(void)
{
    const uint32_t base = 0x81000, count = DISK_SECTORS - base;
    uint8_t cb[16], *ref, *pat;
    uint32_t res;
    int st;

    disk = malloc(512ull * DISK_SECTORS);
    for (uint64_t i = 0; i < 512ull * DISK_SECTORS; i++)
        disk[i] = (uint8_t)(i * 7 + (i >> 9));
    ref = malloc(512ull * DISK_SECTORS);
    memcpy(ref, disk, 512ull * DISK_SECTORS);

    /* --- read only --- */
    CHECK(!msc_start(base, count, 0), "start ro");
    memset(cb, 0, 16);
    cb[0] = 0x12; cb[4] = 36;
    st = run(cb, 6, 36, 1, 0, &res);
    CHECK(st == 0 && inlen == 36 && res == 0 && !memcmp(inbuf + 8, "newfw   ECHO MINI", 17),
          "INQUIRY st %d len %u", st, inlen);
    memset(cb, 0, 16);
    cb[0] = 0x25;
    st = run(cb, 10, 8, 1, 0, &res);
    CHECK(st == 0 && inlen == 8 && inbuf[0] << 24 == (int)((count - 1) & 0xff000000) &&
          (uint32_t)(inbuf[0] << 24 | inbuf[1] << 16 | inbuf[2] << 8 | inbuf[3]) == count - 1,
          "READ CAPACITY");
    memset(cb, 0, 16);
    cb[0] = 0x1a; cb[2] = 0x3f; cb[4] = 192;
    st = run(cb, 6, 192, 1, 0, &res);
    CHECK(st == 0 && inlen == 4 && inbuf[2] == 0x80 && res == 188, "MODE SENSE ro: WP set, residue %u", res);
    rw10(cb, 0x28, 10, 40);
    st = run(cb, 10, 40 * 512, 1, 0, &res);
    CHECK(st == 0 && inlen == 40 * 512 && !memcmp(inbuf, ref + 512ull * (base + 10), 40 * 512),
          "READ 40 sectors");
    pat = malloc(64 * 512);
    for (int i = 0; i < 64 * 512; i++)
        pat[i] = (uint8_t)(0xa5 ^ i);
    rw10(cb, 0x2a, 100, 40);
    st = run(cb, 10, 40 * 512, 0, pat, &res);
    CHECK(st == 1 && writes == 0, "WRITE refused in read only mode (st %d, writes %u)", st, writes);
    memset(cb, 0, 16);
    cb[0] = 0x03; cb[4] = 18;
    st = run(cb, 6, 18, 1, 0, &res);
    CHECK(st == 0 && inbuf[2] == 7 && inbuf[12] == 0x27, "sense DATA PROTECT");

    /* --- read/write --- */
    CHECK(msc_start(0x1000, 100, 1) < 0, "fence: area inside the firmware zone refused");
    CHECK(!msc_start(base, count, 1), "start rw");
    memset(cb, 0, 16);
    cb[0] = 0x1a; cb[2] = 0x3f; cb[4] = 192;
    st = run(cb, 6, 192, 1, 0, &res);
    CHECK(st == 0 && inbuf[2] == 0, "MODE SENSE rw: WP clear");
    rw10(cb, 0x2a, 100, 40);                    /* crosses the 32-sector chunk */
    st = run(cb, 10, 40 * 512, 0, pat, &res);
    CHECK(st == 0 && res == 0 && writes == 40, "WRITE 40 sectors (st %d res %u writes %u)", st, res, writes);
    memcpy(ref + 512ull * (base + 100), pat, 40 * 512);
    rw10(cb, 0x28, 100, 40);
    st = run(cb, 10, 40 * 512, 1, 0, &res);
    CHECK(st == 0 && !memcmp(inbuf, pat, 40 * 512), "read back");
    rw10(cb, 0x2a, count - 1, 2);               /* past the end */
    st = run(cb, 10, 2 * 512, 0, pat, &res);
    CHECK(st == 1 && writes == 40, "WRITE past the end refused");
    rw10(cb, 0x2a, count - 1, 1);               /* last sector */
    st = run(cb, 10, 512, 0, pat + 512, &res);
    CHECK(st == 0 && writes == 41, "WRITE last sector");
    memcpy(ref + 512ull * (base + count - 1), pat + 512, 512);
    rw10(cb, 0x2a, 0, 1);                       /* first sector (MBR) */
    st = run(cb, 10, 512, 0, pat + 1024, &res);
    CHECK(st == 0 && writes == 42, "WRITE first sector");
    memcpy(ref + 512ull * base, pat + 1024, 512);
    rw10(cb, 0x2a, 5, 2);                       /* length mismatch: CBW says 512 */
    st = run(cb, 10, 512, 0, pat, &res);
    CHECK(st == 1 && writes == 42, "WRITE with inconsistent length refused");
    memset(cb, 0, 16);
    cb[0] = 0x1b; cb[4] = 2;                    /* eject */
    st = run(cb, 6, 0, 0, 0, &res);
    CHECK(st == 0 && msc.ejected, "eject");

    CHECK(writes_outside == 0, "writes outside the user volume: %u", writes_outside);
    CHECK(!memcmp(disk, ref, 512ull * DISK_SECTORS), "disk = expected everywhere");
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "all tests passed", failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}
