/* Real-time clock: a PCF8563-type circuit at 0xA2 on I2C1, kept running by the battery while the
 * player is off (ETUDE §28). Read at start-up and every 10 minutes; in between the time follows
 * millis(). */
#ifndef RTC_H
#define RTC_H

#include <stdint.h>

struct rtc_tm {
    uint16_t year;                      /* 2000 to 2099 */
    uint8_t mon, day, hour, min, sec;   /* 1-12, 1-31, 0-23, 0-59, 0-59 */
    uint8_t wday;                       /* 0 = Sunday */
};

int rtc_init(void);                     /* 0, or < 0 when the clock does not answer */
void rtc_service(void);                 /* from the idle loop: read again every 10 minutes */
/* The current time: 1 when it is valid (set since the battery last ran out), 0 when it is not
 * (still given), -1 without a clock */
int rtc_get(struct rtc_tm *t);
int rtc_set(const struct rtc_tm *t);    /* 0, or < 0 */
uint32_t rtc_seconds(const struct rtc_tm *t);       /* since 2000-01-01 00:00:00 */
void rtc_from_seconds(uint32_t s, struct rtc_tm *t);
/* From the newest last-write time of the root directory after a USB session (FAT date << 16 |
 * time, local time of the PC): sets the clock when it is not valid or late by more than a minute.
 * Returns 1 when it was set. */
int rtc_from_fat(uint32_t fat_datetime);

#endif
