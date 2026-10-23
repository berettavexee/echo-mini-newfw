/* PCF8563-type real-time clock at 0xA2 on I2C1 (registers 0x02-0x08: seconds with the VL flag
 * in bit 7, minutes, hours, day, weekday, month with the century in bit 7, year; BCD). The
 * stock firmware reads them at 0x03062106, RECHO 0.10.0 logs them ("RTC 0x51 on I2C1 C"). */
#include "rtc.h"
#include "board.h"
#include "i2c.h"
#include "log.h"

#define RTC_DEV  0xa2
#define RESYNC   600000u                        /* ms */

static int present, valid;
static uint32_t base_s, base_ms, last_read;

static uint8_t bcd(unsigned v) { return (uint8_t)((v / 10) << 4 | v % 10); }
static unsigned dec(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

/* days since 2000-01-01 (2000 to 2099: every fourth year is a leap year) */
static uint32_t days(unsigned y, unsigned m, unsigned d)
{
    static const uint16_t cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint32_t n = (y - 2000) * 365 + (y - 2000 + 3) / 4 + cum[m - 1] + d - 1;
    if (m > 2 && y % 4 == 0)
        n++;
    return n;
}

uint32_t rtc_seconds(const struct rtc_tm *t)
{
    return days(t->year, t->mon, t->day) * 86400u + t->hour * 3600u + t->min * 60u + t->sec;
}

void rtc_from_seconds(uint32_t s, struct rtc_tm *t)
{
    uint32_t d = s / 86400, r = s % 86400;
    t->hour = (uint8_t)(r / 3600);
    t->min = (uint8_t)(r / 60 % 60);
    t->sec = (uint8_t)(r % 60);
    t->wday = (uint8_t)((d + 6) % 7);           /* 2000-01-01 was a Saturday */
    unsigned y = 2000;
    for (;;) {
        unsigned len = y % 4 ? 365 : 366;
        if (d < len)
            break;
        d -= len;
        y++;
    }
    static const uint8_t ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned m = 0;
    for (;;) {
        unsigned len = ml[m] + (m == 1 && y % 4 == 0);
        if (d < len)
            break;
        d -= len;
        m++;
    }
    t->year = (uint16_t)y;
    t->mon = (uint8_t)(m + 1);
    t->day = (uint8_t)(d + 1);
}

static int sane(const struct rtc_tm *t)
{
    return t->mon >= 1 && t->mon <= 12 && t->day >= 1 && t->day <= 31 && t->hour < 24 &&
           t->min < 60 && t->sec < 60;
}

/* Registers 0x02-0x08 into base_s; returns 0, or < 0 */
static int read_clock(const char *why)
{
    uint8_t r[7];
    uint32_t t0 = ticks();
    int e = i2c1_read(RTC_DEV, 0x02, r, 7);
    if (e) {
        log_printf("rtc (%s): read error %d", why, e);
        return e;
    }
    struct rtc_tm t = {
        .year = (uint16_t)(2000 + dec(r[6])), .mon = (uint8_t)dec(r[5] & 0x1f),
        .day = (uint8_t)dec(r[3] & 0x3f), .hour = (uint8_t)dec(r[2] & 0x3f),
        .min = (uint8_t)dec(r[1] & 0x7f), .sec = (uint8_t)dec(r[0] & 0x7f), .wday = r[4] & 7,
    };
    if (!sane(&t)) {
        log_printf("rtc (%s): impossible time %02x %02x %02x %02x %02x %02x %02x", why, r[0], r[1],
                   r[2], r[3], r[4], r[5], r[6]);
        return -1;
    }
    base_s = rtc_seconds(&t);
    base_ms = last_read = millis();
    valid = !(r[0] & 0x80);                     /* VL: the clock stopped since it was last set */
    if (why)
        log_printf("rtc (%s): %04u-%02u-%02u %02u:%02u:%02u%s (%u us)", why, t.year, t.mon, t.day,
                   t.hour, t.min, t.sec, valid ? "" : ", not set (VL)", ticks() - t0);
    return 0;
}

int rtc_init(void)
{
    i2c1_init();
    present = !read_clock("boot");
    return present ? 0 : -1;
}

void rtc_service(void)
{
    if (present && millis() - last_read > RESYNC)
        read_clock(0);
}

int rtc_get(struct rtc_tm *t)
{
    if (!present)
        return -1;
    rtc_from_seconds(base_s + (millis() - base_ms) / 1000, t);
    return valid;
}

int rtc_set(const struct rtc_tm *in)
{
    struct rtc_tm t;
    rtc_from_seconds(rtc_seconds(in), &t);      /* normalised, with the weekday */
    uint8_t r[7] = { bcd(t.sec), bcd(t.min), bcd(t.hour), bcd(t.day), t.wday, bcd(t.mon),
                     bcd(t.year % 100) };      /* VL cleared, century bit 0 (20xx) */
    int e = i2c1_write(RTC_DEV, 0x02, r, 7);
    log_printf("rtc: set to %04u-%02u-%02u %02u:%02u:%02u (%d)", t.year, t.mon, t.day, t.hour,
               t.min, t.sec, e);
    if (!e) {
        present = 1;
        e = read_clock("after set");
    }
    return e;
}

int rtc_from_fat(uint32_t v)
{
    struct rtc_tm t = {
        .year = (uint16_t)(1980 + (v >> 25)), .mon = (uint8_t)(v >> 21 & 15), .day = (uint8_t)(v >> 16 & 31),
        .hour = (uint8_t)(v >> 11 & 31), .min = (uint8_t)(v >> 5 & 63), .sec = (uint8_t)((v & 31) * 2),
    };
    struct rtc_tm now;
    if (!present || t.year < 2024 || t.year > 2099 || !sane(&t))
        return 0;
    int ok = rtc_get(&now);
    uint32_t f = rtc_seconds(&t), n = rtc_seconds(&now);
    if (ok == 1 && f <= n + 60)
        return 0;                               /* valid and not late: left alone */
    log_printf("rtc: newest file written %04u-%02u-%02u %02u:%02u:%02u, clock %s", t.year, t.mon,
               t.day, t.hour, t.min, t.sec, ok == 1 ? "late" : "not set");
    return !rtc_set(&t);
}
