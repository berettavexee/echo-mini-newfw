/* Host-side test of src/log.c: ring, file attach, flush, wrap, re-attach after a reboot.
 *   cc -O1 -Wall -Isrc -o build/log_test tools/log_test.c && build/log_test OUT.txt
 * With -DLOG_TOKENS (ETUDE §133) log_printf writes token records, made text at the flush from
 * the section in memory: the file must come out the same as without (check.sh compares them);
 * then records whose formats are gone come out raw. */
#define _GNU_SOURCE                             /* memmem */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t ring_mem[0x3000] __attribute__((aligned(4)));
#define LOG_RING ((uintptr_t)ring_mem)
#define BOARD_H
#define EMMC_H
#define FAT_H
static uint32_t now_us, now_ms;
uint32_t ticks(void) { return now_us; }
uint32_t millis(void) { return now_ms; }

#define FILE_FIRST 0x90000u
#define FILE_SECTORS 16u
static uint8_t file[FILE_SECTORS * 512];
static uint32_t lo, hi, bad_writes;
struct fat_file { char name[13]; uint32_t cluster, size, pos, cur; };
int fat_find_root(const char *name, struct fat_file *f) { (void)name; f->size = sizeof file; return 0; }
int fat_extent(const struct fat_file *f, uint32_t *first, uint32_t *count)
{ (void)f; *first = FILE_FIRST; *count = FILE_SECTORS; return 0; }
int emmc_log_area(uint32_t base, uint32_t count) { lo = base; hi = base + count; return count ? 0 : -6; }
int emmc_write_log(uint32_t s, uint32_t n, const void *buf)
{
    if (s < lo || s + n > hi || s < FILE_FIRST) { bad_writes++; return -6; }
    memcpy(file + 512 * (s - FILE_FIRST), buf, 512 * n);
    return 0;
}
int emmc_read(uint32_t s, uint32_t n, void *buf)
{
    memcpy(buf, file + 512 * (s - FILE_FIRST), 512 * n);
    return 0;
}

#ifdef LOG_TOKENS
#define LOG_TOKENS_HOST
uint32_t log_host_fmt = 0x1234;
#endif
#include "../src/log.c"

int main(int argc, char **argv)
{
    int fail = 0;
    memset(file, ' ', sizeof file);
    log_init();
    log_printf("first boot %d", 1);
    if (log_attach()) { printf("attach failed\n"); return 1; }
    log_flush();
    for (int i = 0; i < 300; i++) {             /* ~ 300 * 40 bytes = 12 KB: wraps 15 sectors */
        now_ms += 7;
        log_printf("line %04d value %x", i, i * 77);
        log_poll();
        now_us += 600000;
    }
    log_flush();
    log_printf("before reboot");                /* not flushed: stays in the ring */
    /* reboot: ring kept, file state re-read from the header */
    log_init();
    if (logs.recovered == 0) { printf("FAIL nothing recovered\n"); fail++; }
    log_printf("second boot");
    log_attach();
    log_flush();
    static const char long_s[] = "0123456789012345678901234567890123456789012345678901234567"
                                 "8901234567890123456789";
    const char *nul = 0;
    log_printf("strings '%s' %d '%s' '%5s' %c %08X", long_s, -5, nul, "ab", 'z', 0xbeefu);
#ifdef LOG_TOKENS
    log_flush();
    log_printf("formats gone %u '%s'", 7u, "x");    /* written raw: the image changed */
    log_host_fmt = 0x5678;
    log_flush();
    log_host_fmt = 0x1234;
    log_printf("formats gone %u '%s'", 7u, "x");    /* the same line from a raw one */
#else
    log_text("?log 0000 7 \"x\"");                  /* what the raw line looks like */
    log_printf("formats gone %u '%s'", 7u, "x");
#endif
    log_flush();
    FILE *f = fopen(argc > 1 ? argv[1] : "build/log_test.txt", "wb");
    fwrite(file, 1, sizeof file, f);
    fclose(f);
    if (bad_writes) { printf("FAIL %u writes outside the file\n", bad_writes); fail++; }
#ifdef LOG_TOKENS
    static const char *want[] = {                   /* raw, then text again */
        "?log ", " 7 \"x\"\n", "formats gone 7 'x'\n" };
    for (int i = 0; i < 3; i++)
        if (!memmem(file, sizeof file, want[i], strlen(want[i]))) {
            printf("FAIL not in the file: %s\n", want[i]);
            fail++;
        }
#endif
    printf("wraps %u next %u boots %u lost %u\n", logs.wraps, logs.next, logs.boots, logs.lost);
    return fail;
}
