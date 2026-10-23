/* libopus allocation for our core B runtime (included by os_support.h with CUSTOM_SUPPORT):
 * the decoder state is a static buffer (opus_decoder_init), the pseudo-stack a static array */
#ifndef CUSTOM_SUPPORT_H
#define CUSTOM_SUPPORT_H
#include <stddef.h>

extern char opus_scratch[];
#define OVERRIDE_OPUS_ALLOC_SCRATCH
static inline void *opus_alloc_scratch(size_t size)
{
    (void)size;
    return opus_scratch;
}

#define OVERRIDE_OPUS_ALLOC
static inline void *opus_alloc(size_t size)
{
    (void)size;
    return NULL;
}

#define OVERRIDE_OPUS_FREE
static inline void opus_free(void *ptr)
{
    (void)ptr;
}
#endif
