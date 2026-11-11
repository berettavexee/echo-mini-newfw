/* Firmware log: lines kept in a ring in PMU-SRAM (it survives a reset, so the lines written
 * before a fault or a reboot are not lost), copied from time to time into NEWFWLOG.TXT at the
 * root of the user volume.
 *
 * The file is created once on the computer (any content, contiguous, 8 KiB to 16 MiB) and is
 * only ever overwritten in place: the firmware never allocates clusters nor touches the FAT or
 * the directory. Its sector 0 is a text header giving where the next line goes; the data
 * sectors after it form a circular text log (tools/readlog.py prints it in order). */
#ifndef LOG_H
#define LOG_H

#include <stdint.h>

#define LOG_FILE "NEWFWLOG.TXT"

struct log_state {
    int attached;               /* the file is open for writing */
    int error;                  /* last attach / write error (0 = none) */
    uint32_t file_sectors;      /* data sectors of the file, header included */
    uint32_t next;              /* sector being filled */
    uint32_t wraps;
    uint32_t boots;             /* boots seen by the ring since it was created */
    uint32_t recovered;         /* bytes left unwritten by the previous boot */
    uint32_t lost;              /* bytes dropped because the ring overflowed */
};

extern struct log_state logs;

void log_init(void);            /* at boot, before anything is logged */
/* A word for the next boot, kept in PMU-SRAM across a reset; taken (and cleared) once */
void log_boot_note(uint32_t v);
uint32_t log_boot_note_take(void);
/* log_printf(fmt, ...): %s %c %d %u %x, width, 0 flag; arguments of 32 bits or strings.
 * Tokens (ETUDE §133): the format is not kept in the code loaded. The macro puts it in the
 * section "logfmt", never loaded, which tools/mkimg.py stores in the image (header +0x1e8); the
 * ring keeps the time, the format's offset in that section and the arguments (strings copied; a
 * record 256 bytes at most, as a line of text). The text is made when the ring is written into
 * NEWFWLOG.TXT, from the formats read in the image on the eMMC; lines it cannot make (a firmware
 * whose formats are not in the image any more: after an install) are written raw ("?log 01a4
 * ..."), for readlog.py --elf.
 * Whether each argument is a string is told by its type (_Generic), at compile time.
 * log_text(): the line made at once (text in the ring): the installer's, whose image is being
 * rewritten when the ring is written out.
 * tools/check.sh checks the formats with LOG_FORMAT_CHECK (then log_printf is a function), with
 * uint32_t as unsigned int: it is unsigned long here, which %u prints all the same. Host builds
 * have the function too, unless LOG_TOKENS (log_test). */
#ifdef LOG_FORMAT_CHECK
__attribute__((format(printf, 1, 2)))
#endif
void log_text(const char *fmt, ...);
#if !defined(LOG_FORMAT_CHECK) && (defined(__arm__) || defined(LOG_TOKENS))
extern const char __start_logfmt[];
void log_tok(const char *fmt, uint32_t desc, ...);
#define LOG_S_(x)   _Generic((x), char *: 1u, const char *: 1u, default: 0u)
#define LOG_N_(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, \
               _18, _19, _20, _21, _22, _23, _24, n, ...) n
#define LOG_N(...)  LOG_N_(0, ##__VA_ARGS__, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, \
                           11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define LOG_M0()    0u
#define LOG_M1(a)   LOG_S_(a)
#define LOG_M2(a, ...) (LOG_S_(a) | LOG_M1(__VA_ARGS__) << 1)
#define LOG_M3(a, ...) (LOG_S_(a) | LOG_M2(__VA_ARGS__) << 1)
#define LOG_M4(a, ...) (LOG_S_(a) | LOG_M3(__VA_ARGS__) << 1)
#define LOG_M5(a, ...) (LOG_S_(a) | LOG_M4(__VA_ARGS__) << 1)
#define LOG_M6(a, ...) (LOG_S_(a) | LOG_M5(__VA_ARGS__) << 1)
#define LOG_M7(a, ...) (LOG_S_(a) | LOG_M6(__VA_ARGS__) << 1)
#define LOG_M8(a, ...) (LOG_S_(a) | LOG_M7(__VA_ARGS__) << 1)
#define LOG_M9(a, ...) (LOG_S_(a) | LOG_M8(__VA_ARGS__) << 1)
#define LOG_M10(a, ...) (LOG_S_(a) | LOG_M9(__VA_ARGS__) << 1)
#define LOG_M11(a, ...) (LOG_S_(a) | LOG_M10(__VA_ARGS__) << 1)
#define LOG_M12(a, ...) (LOG_S_(a) | LOG_M11(__VA_ARGS__) << 1)
#define LOG_M13(a, ...) (LOG_S_(a) | LOG_M12(__VA_ARGS__) << 1)
#define LOG_M14(a, ...) (LOG_S_(a) | LOG_M13(__VA_ARGS__) << 1)
#define LOG_M15(a, ...) (LOG_S_(a) | LOG_M14(__VA_ARGS__) << 1)
#define LOG_M16(a, ...) (LOG_S_(a) | LOG_M15(__VA_ARGS__) << 1)
#define LOG_M17(a, ...) (LOG_S_(a) | LOG_M16(__VA_ARGS__) << 1)
#define LOG_M18(a, ...) (LOG_S_(a) | LOG_M17(__VA_ARGS__) << 1)
#define LOG_M19(a, ...) (LOG_S_(a) | LOG_M18(__VA_ARGS__) << 1)
#define LOG_M20(a, ...) (LOG_S_(a) | LOG_M19(__VA_ARGS__) << 1)
#define LOG_M21(a, ...) (LOG_S_(a) | LOG_M20(__VA_ARGS__) << 1)
#define LOG_M22(a, ...) (LOG_S_(a) | LOG_M21(__VA_ARGS__) << 1)
#define LOG_M23(a, ...) (LOG_S_(a) | LOG_M22(__VA_ARGS__) << 1)
#define LOG_M24(a, ...) (LOG_S_(a) | LOG_M23(__VA_ARGS__) << 1)
#define LOG_CAT_(a, b) a##b
#define LOG_CAT(a, b)  LOG_CAT_(a, b)
/* bits 0-4: the number of arguments (24 at most); bit 5 + i: argument i is a string */
#define LOG_DESC(...) (LOG_N(__VA_ARGS__) | LOG_CAT(LOG_M, LOG_N(__VA_ARGS__))(__VA_ARGS__) << 5)
#define log_printf(fmt, ...)                                                                    \
    log_tok(({ static const char log_f_[] __attribute__((section("logfmt"), used)) = fmt;       \
               log_f_; }), LOG_DESC(__VA_ARGS__), ##__VA_ARGS__)
#else
#ifdef LOG_FORMAT_CHECK
__attribute__((format(printf, 1, 2)))
#endif
void log_printf(const char *fmt, ...);   /* host builds: log_text() */
#endif
int log_attach(void);           /* finds the file (after the volume is mounted) */
void log_detach(void);          /* writes what is pending, then stops writing (USB mode) */
int log_flush(void);            /* writes the pending lines now */
void log_poll(void);            /* writes them when enough time or text has gone by */
/* The last sector of the log file is not part of the ring: it keeps the resume state of the
 * user interface (512 bytes, starting with "NEWFWSTATE", ui.c) */
int log_state_read(void *buf);
int log_state_write(const void *buf);
/* The sector before it (0.99r32): the fault record, starting with "NEWFWCRASH" (crash.c) */
int log_crash_read(void *buf);
int log_crash_write(const void *buf);
/* log_printf's formats into a buffer; returns the end of the text (not terminated) */
#ifdef LOG_FORMAT_CHECK
__attribute__((format(printf, 3, 4)))
#endif
char *log_format(char *p, char *end, const char *f, ...);

#endif
