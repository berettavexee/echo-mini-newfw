/* The play queue kept on the internal memory, NEWFWQ.DAT (qfile.c, ETUDE §128) */
#ifndef QFILE_H
#define QFILE_H

#include <stdint.h>

#define QF_MAX 24576u                           /* entries */

struct entry;
int qf_open(void);                      /* found or created, its header read: 0, or -1 */
void qf_forget(void);                   /* opened again at the next use (after USB) */
uint32_t qf_count(void);                /* the entries of the queue in use */
uint32_t qf_gen(void);                  /* one more at each queue written */
int qf_get(uint32_t i, struct entry *e, char *name, int max);
/* Writing: qf_begin() a new queue (the other half) or qf_append() after the last entry, then
 * qf_put() each entry, then qf_end(1) (the header: the queue in use) or qf_end(0) (dropped) */
int qf_begin(void);
int qf_append(void);
int qf_put(const struct entry *e, const char *name);
uint32_t qf_written(void);
int qf_end(int keep);
int qf_insert(uint32_t at, uint32_t n, int (*put)(uint32_t k, void *ctx), void *ctx);

#endif
