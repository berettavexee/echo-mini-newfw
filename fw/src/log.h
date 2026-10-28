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
/* tools/check.sh checks the formats with LOG_FORMAT_CHECK (and uint32_t as unsigned int: it is
 * unsigned long here, which %u prints all the same) */
#ifdef LOG_FORMAT_CHECK
__attribute__((format(printf, 1, 2)))
#endif
void log_printf(const char *fmt, ...);   /* %s %c %d %u %x, width, 0 flag */
int log_attach(void);           /* finds the file (after the volume is mounted) */
void log_detach(void);          /* writes what is pending, then stops writing (USB mode) */
int log_flush(void);            /* writes the pending lines now */
void log_poll(void);            /* writes them when enough time or text has gone by */
/* The last sector of the log file is not part of the ring: it keeps the resume state of the
 * user interface (512 bytes, starting with "NEWFWSTATE", ui.c) */
int log_state_read(void *buf);
int log_state_write(const void *buf);

#endif
