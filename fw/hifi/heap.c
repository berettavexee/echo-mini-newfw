/* malloc/free for decoders that need them (Tremor): first-fit over a static array, coalescing
 * on free; heap_reset() empties it at every OPEN. From our module 77 port (re/vorbis/port77).
 * The array is in .noinit, which hp_reset() does not clear: what lies below a floor (Vorbis
 * codebooks) can be kept from one track to the next, see heap_keep()/heap_resume(). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef HEAP_SIZE
#define HEAP_SIZE (88 * 1024)
#endif

typedef struct { uint32_t size, used; } blk_t;      /* size includes the header */
static uint8_t heap[HEAP_SIZE] __attribute__((aligned(8), section(".noinit")));
static uint32_t heap_top, heap_floor;                /* malloc and free work above the floor */
uint32_t heap_peak, heap_cur;

void heap_reset(void)
{
    heap_top = heap_cur = heap_floor = 0;
}

/* The blocks allocated so far are kept: later allocations go above them. Returns the floor. */
uint32_t heap_keep(void)
{
    heap_floor = heap_top;
    return heap_floor;
}

/* Back to a heap whose blocks below 'floor' (heap_cur bytes of them in use) were kept */
void heap_resume(uint32_t floor, uint32_t cur)
{
    heap_top = heap_floor = floor;
    heap_cur = cur;
    if (heap_cur > heap_peak)
        heap_peak = heap_cur;
}

const uint8_t *heap_base(void)
{
    return heap;
}

void *malloc(size_t n)
{
    uint32_t need = (uint32_t)((n + 7) & ~(size_t)7) + sizeof(blk_t);
    uint8_t *p = heap + heap_floor;
    blk_t *b;
    while (p < heap + heap_top) {
        b = (blk_t *)p;
        if (!b->used && b->size >= need) {
            if (b->size >= need + 16) {
                blk_t *r = (blk_t *)(p + need);
                r->size = b->size - need;
                r->used = 0;
                b->size = need;
            }
            goto take;
        }
        p += b->size;
    }
    if (heap_top + need > HEAP_SIZE)
        return 0;
    b = (blk_t *)(heap + heap_top);
    b->size = need;
    heap_top += need;
take:
    b->used = 1;
    heap_cur += b->size;
    if (heap_cur > heap_peak)
        heap_peak = heap_cur;
    return (uint8_t *)b + sizeof(blk_t);
}

void free(void *ptr)
{
    if (!ptr)
        return;
    blk_t *b = (blk_t *)((uint8_t *)ptr - sizeof(blk_t));
    b->used = 0;
    heap_cur -= b->size;
    for (uint8_t *p = heap + heap_floor; p < heap + heap_top;) {   /* coalesce, trim the top */
        blk_t *x = (blk_t *)p;
        if (!x->used) {
            while (p + x->size < heap + heap_top && !((blk_t *)(p + x->size))->used)
                x->size += ((blk_t *)(p + x->size))->size;
            if (p + x->size == heap + heap_top) {
                heap_top = (uint32_t)(p - heap);
                break;
            }
        }
        p += x->size;
    }
}

void *calloc(size_t a, size_t b)
{
    void *p = malloc(a * b);
    if (p)
        memset(p, 0, a * b);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr)
        return malloc(n);
    blk_t *b = (blk_t *)((uint8_t *)ptr - sizeof(blk_t));
    if (b->size - sizeof(blk_t) >= n)
        return ptr;
    void *q = malloc(n);
    if (q) {
        memcpy(q, ptr, b->size - sizeof(blk_t));
        free(ptr);
    }
    return q;
}
