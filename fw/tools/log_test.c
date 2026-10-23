/* Host-side test of src/log.c: ring, file attach, flush, wrap, re-attach after a reboot.
 *   cc -O1 -Wall -Isrc -o build/log_test tools/log_test.c && build/log_test */
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

#include "../src/log.c"

int main(void)
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
    FILE *f = fopen("build/log_test.txt", "wb");
    fwrite(file, 1, sizeof file, f);
    fclose(f);
    if (bad_writes) { printf("FAIL %u writes outside the file\n", bad_writes); fail++; }
    printf("wraps %u next %u boots %u lost %u\n", logs.wraps, logs.next, logs.boots, logs.lost);
    return fail;
}
