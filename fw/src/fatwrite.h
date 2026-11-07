/* A contiguous file created at the root of the internal FAT32 volume (ETUDE §124): fatwrite.c */
#ifndef FATWRITE_H
#define FATWRITE_H

#include <stdint.h>
#include "fat.h"

enum { FATW_EXISTS = 1, FATW_EIO = -1, FATW_ENOVOL = -2, FATW_ENOSPACE = -3,
       FATW_EPROTECT = -4 };

/* name: an 8.3 name in capitals ("NEWFWLIB.DAT"); size bytes in one run of clusters, its first
 * sector 'first' (512 bytes), the rest left as it was. 0 and f, the new file; FATW_EXISTS and f,
 * a file of that name (left untouched); < 0 nothing created (or, after a write error, at most
 * clusters reserved that nothing uses). Not during a USB session (the host owns the volume). */
int fat_create_contig(const char *name, uint32_t size, const void *first, struct fat_file *f);
extern void (*fatw_yield)(void);    /* called while the FAT is searched, when set */

#endif
