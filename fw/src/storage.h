/* The storage task (rework step 3, ETUDE §100): long reads (the next track's tags, a tree walk)
 * run as jobs in a coroutine of their own, a few milliseconds each pass of the main loop, so that
 * the screens and the keys go on meanwhile (the audio path is served by the loop as always).
 *
 * The coroutine yields only between two calls of the file layer (storage_yield(), called by the
 * readers between two reads), never inside one: fat.c and the drivers keep static buffers and
 * caches, which the loop's own reads (the player's input) use in between. A job that needs a
 * reader the loop may use too (tags.c) is drained first by the loop (storage_drain()). */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>

typedef void (*storage_job)(void *arg);

int storage_submit(storage_job fn, void *arg);  /* queued: 0; the queue full: -1 */
int storage_busy(void);                         /* a job queued or running */
void storage_run(uint32_t budget_us);           /* the main loop: the task for up to budget_us */
void storage_drain(void);                       /* every job to its end, now (blocking) */
void storage_yield(void);                       /* in a job: back to the loop, budget spent */
int storage_in_task(void);                      /* called from inside a job */
uint32_t storage_stack_free(void);              /* bytes of the task's stack never used */

#endif
