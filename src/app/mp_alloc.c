/*
 * The heap of the player: a boundary-tag allocator over up to four memory
 * regions (the retro-go pools are bump allocators without free()). On the
 * console: what is left of RAM_EMU after the binary, the LCD memory freed
 * by the LUT8 mode, DTCM below the stack, ITCM.
 *
 * Every block has a 4 byte header and a 4 byte footer holding its size
 * (multiple of 8) with bit 0 = "in use". Each region starts with a "used"
 * footer and ends with a "used" header (sentinels): blocks never merge
 * across regions. Free blocks are in one doubly linked list; malloc() takes
 * the best fit (the modules want big blocks: keep them whole).
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "mp_platform.h"

#define ALIGN      8u
#define HDR        4u
#define MIN_BLOCK  24u   /* header + 2 links + footer, rounded to ALIGN */
#define USED       1u
#define MAX_REGIONS 4

typedef struct free_node {
    struct free_node *prev;
    struct free_node *next;
} free_node_t;

typedef struct {
    uint8_t *lo, *hi;   /* first block header / end sentinel */
} region_t;

static region_t regions[MAX_REGIONS];
static int nregions;
static free_node_t *free_list;
static size_t heap_used, heap_peak, heap_total;

static inline uint32_t blk_size(uint8_t *b) { return *(uint32_t *)b & ~7u; }
static inline int blk_used(uint8_t *b) { return *(uint32_t *)b & USED; }
static inline void blk_set(uint8_t *b, uint32_t s, int used)
{
    *(uint32_t *)b = s | (used ? USED : 0);
    *(uint32_t *)(b + s - HDR) = s | (used ? USED : 0);
}
static inline free_node_t *blk_node(uint8_t *b) { return (free_node_t *)(b + HDR); }
static inline uint8_t *node_blk(free_node_t *n) { return (uint8_t *)n - HDR; }

static void list_remove(free_node_t *n)
{
    if (n->prev)
        n->prev->next = n->next;
    else
        free_list = n->next;
    if (n->next)
        n->next->prev = n->prev;
}

static void list_push(free_node_t *n)
{
    n->prev = NULL;
    n->next = free_list;
    if (free_list)
        free_list->prev = n;
    free_list = n;
}

#ifdef MP_HOST_SYSTEM_MALLOC
/* Host debugging: the system allocator, so that AddressSanitizer sees every
 * block. */
#include <stdlib.h>
void mp_heap_init(void) { heap_total = 0; }
bool mp_heap_add(void *base, size_t size) { (void)base; heap_total += size; return true; }
void *mp_malloc(size_t size) { return malloc(size ? size : 1); }
void mp_free(void *ptr) { free(ptr); }
void *mp_calloc(size_t n, size_t size) { return calloc(n ? n : 1, size ? size : 1); }
void *mp_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
size_t mp_heap_used(void) { return 0; }
size_t mp_heap_peak(void) { return 0; }
size_t mp_heap_size(void) { return heap_total; }
size_t mp_heap_largest(void) { return heap_total; }
#else
void mp_heap_init(void)
{
    nregions = 0;
    free_list = NULL;
    heap_used = heap_peak = heap_total = 0;
}

bool mp_heap_add(void *base, size_t size)
{
    if (nregions == MAX_REGIONS || size < 256)
        return false;
    /* payloads 8 aligned: headers 4 bytes before an 8 boundary; a "used"
     * footer before the first block */
    uintptr_t lo = ((uintptr_t)base + HDR + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    lo += ALIGN - HDR;
    uintptr_t hi = ((uintptr_t)base + size) & ~(uintptr_t)(ALIGN - 1);
    hi -= HDR; /* the end sentinel */
    if (hi <= lo + MIN_BLOCK)
        return false;
    uint32_t total = (uint32_t)((hi - lo) & ~(uintptr_t)(ALIGN - 1));
    region_t *r = &regions[nregions++];
    r->lo = (uint8_t *)lo;
    r->hi = r->lo + total;
    *(uint32_t *)(r->lo - HDR) = USED; /* start sentinel (a used footer) */
    *(uint32_t *)r->hi = USED;         /* end sentinel: size 0, used */
    blk_set(r->lo, total, 0);
    list_push(blk_node(r->lo));
    heap_total += total;
    return true;
}

static inline int in_heap(const void *p)
{
    for (int i = 0; i < nregions; i++)
        if ((const uint8_t *)p > regions[i].lo && (const uint8_t *)p < regions[i].hi)
            return 1;
    return 0;
}

void *mp_malloc(size_t size)
{
    if (size == 0)
        size = 1;
    if (size > 0x7fffff00u)
        return NULL;
    uint32_t need = (uint32_t)((size + 2 * HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1));
    if (need < MIN_BLOCK)
        need = MIN_BLOCK;
    free_node_t *best = NULL;
    uint32_t best_size = 0;
    for (free_node_t *n = free_list; n; n = n->next) {
        uint32_t s = blk_size(node_blk(n));
        if (s >= need && (!best || s < best_size)) {
            best = n;
            best_size = s;
            if (s == need)
                break;
        }
    }
    if (!best)
        return NULL;
    uint8_t *b = node_blk(best);
    uint32_t s = best_size;
    list_remove(best);
    if (s - need >= MIN_BLOCK) {
        uint8_t *rest = b + need;
        blk_set(rest, s - need, 0);
        list_push(blk_node(rest));
        s = need;
    }
    blk_set(b, s, 1);
    heap_used += s;
    if (heap_used > heap_peak)
        heap_peak = heap_used;
    return b + HDR;
}

void mp_free(void *ptr)
{
    if (ptr == NULL || !in_heap(ptr))
        return;
    uint8_t *b = (uint8_t *)ptr - HDR;
    if (!blk_used(b)) {
        printf("mp: double free %p\n", ptr);
        return;
    }
    uint32_t s = blk_size(b);
    heap_used -= s;
    uint8_t *next = b + s;
    if (!blk_used(next)) {
        list_remove(blk_node(next));
        s += blk_size(next);
    }
    uint32_t ptag = *(uint32_t *)(b - HDR);
    if (!(ptag & USED)) {
        uint8_t *prev = b - (ptag & ~7u);
        list_remove(blk_node(prev));
        s += blk_size(prev);
        b = prev;
    }
    blk_set(b, s, 0);
    list_push(blk_node(b));
}

void *mp_calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    size_t total = n * size;
    void *p = mp_malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *mp_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)
        return mp_malloc(size);
    if (size == 0) {
        mp_free(ptr);
        return NULL;
    }
    size_t old = blk_size((uint8_t *)ptr - HDR) - 2 * HDR;
    if (old >= size)
        return ptr;
    /* grow in place into a free neighbour */
    uint8_t *b = (uint8_t *)ptr - HDR;
    uint32_t s = blk_size(b);
    uint8_t *next = b + s;
    uint32_t need = (uint32_t)((size + 2 * HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1));
    if (!blk_used(next) && s + blk_size(next) >= need) {
        uint32_t total = s + blk_size(next);
        list_remove(blk_node(next));
        heap_used -= s;
        if (total - need >= MIN_BLOCK) {
            blk_set(b + need, total - need, 0);
            list_push(blk_node(b + need));
            total = need;
        }
        blk_set(b, total, 1);
        heap_used += total;
        if (heap_used > heap_peak)
            heap_peak = heap_used;
        return ptr;
    }
    void *np = mp_malloc(size);
    if (np == NULL)
        return NULL;
    memcpy(np, ptr, old);
    mp_free(ptr);
    return np;
}

size_t mp_heap_used(void) { return heap_used; }
size_t mp_heap_peak(void) { return heap_peak; }
size_t mp_heap_size(void) { return heap_total; }

size_t mp_heap_largest(void)
{
    uint32_t best = 0;
    for (free_node_t *n = free_list; n; n = n->next) {
        uint32_t s = blk_size(node_blk(n));
        if (s > best)
            best = s;
    }
    return best > 2 * HDR ? best - 2 * HDR : 0;
}
#endif
