/* malloc — a first-fit free list over a break-grown heap.
 *
 * Why this and not something cleverer: the allocator a C library ships has
 * to be correct on every target before it is fast on any, and a first-fit
 * list with coalescing is small enough to read in one sitting and to reason
 * about when a target misbehaves. It is the same judgement `src/ir` made
 * about the IR — correct and small first; the shape does not prevent a
 * better one later, because nothing outside this file knows the shape.
 *
 * Blocks carry a header with their size and a free flag. Adjacent free
 * blocks coalesce on free, which is what keeps a long-running program from
 * sawing its heap into unusable crumbs.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../../os/backend.h"

/* Every block is aligned for any type: max_align_t's alignment, which on
 * both targets is 16 (long double / __int128). */
#define ALIGN 16UL
#define ALIGN_UP(n) (((n) + (ALIGN - 1)) & ~(ALIGN - 1))

struct blk {
    size_t size;            /* payload bytes, not counting this header */
    struct blk *next;       /* address order, so coalescing is a neighbour test */
    int free;
};

#define HDR ALIGN_UP(sizeof(struct blk))

static struct blk *g_head;

/* Ask the OS for at least `need` more bytes, in chunks so that a program
 * doing many small allocations does not make one syscall each. */
static struct blk *grow(size_t need)
{
    size_t chunk = ALIGN_UP(need + HDR);
    if (chunk < 64 * 1024)
        chunk = 64 * 1024;
    void *p = __os_sbrk((long)chunk);
    if (p == (void *)-1) {
        errno = ENOMEM;
        return NULL;
    }
    struct blk *b = p;
    b->size = chunk - HDR;
    b->next = NULL;
    b->free = 1;
    if (!g_head) {
        g_head = b;
    } else {
        struct blk *t = g_head;
        while (t->next) t = t->next;
        t->next = b;
        /* The new block may abut the last one: join them rather than leave
         * a seam no later coalesce can cross. */
        if ((unsigned char *)t + HDR + t->size == (unsigned char *)b &&
            t->free) {
            t->size += HDR + b->size;
            t->next = b->next;
            return t;
        }
    }
    return b;
}

static void split(struct blk *b, size_t want)
{
    /* Only worth splitting if the remainder can hold a header and enough
     * payload to be handed out; otherwise the tail is left as slack. */
    if (b->size < want + HDR + ALIGN)
        return;
    struct blk *rest = (struct blk *)((unsigned char *)b + HDR + want);
    rest->size = b->size - want - HDR;
    rest->free = 1;
    rest->next = b->next;
    b->size = want;
    b->next = rest;
}

void *malloc(size_t n)
{
    if (n == 0)
        n = 1;                 /* a unique address, as C allows and callers expect */
    size_t want = ALIGN_UP(n);
    if (want < n) {            /* the rounding overflowed: no such allocation */
        errno = ENOMEM;
        return NULL;
    }
    for (struct blk *b = g_head; b; b = b->next)
        if (b->free && b->size >= want) {
            split(b, want);
            b->free = 0;
            return (unsigned char *)b + HDR;
        }
    struct blk *b = grow(want);
    if (!b)
        return NULL;
    split(b, want);
    b->free = 0;
    return (unsigned char *)b + HDR;
}

void free(void *p)
{
    if (!p)
        return;                /* free(NULL) is defined and does nothing */
    struct blk *b = (struct blk *)((unsigned char *)p - HDR);
    b->free = 1;
    /* Coalesce forward across every free neighbour, then let the next free
     * of an earlier block join this one. A single forward pass is enough
     * because the list is in address order. */
    while (b->next && b->next->free &&
           (unsigned char *)b + HDR + b->size == (unsigned char *)b->next) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
    }
    for (struct blk *t = g_head; t && t != b; t = t->next)
        if (t->free && t->next == b &&
            (unsigned char *)t + HDR + t->size == (unsigned char *)b) {
            t->size += HDR + b->size;
            t->next = b->next;
            break;
        }
}

void *calloc(size_t n, size_t size)
{
    /* The multiplication is the whole point of calloc's signature: a
     * product that overflows must fail, not wrap and under-allocate. */
    if (size && n > (size_t)-1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t total = n * size;
    void *p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    if (n == 0) { free(p); return NULL; }
    struct blk *b = (struct blk *)((unsigned char *)p - HDR);
    size_t want = ALIGN_UP(n);
    if (b->size >= want) {
        split(b, want);
        return p;
    }
    /* Grow in place when the next block is free and adjacent — which is the
     * common case for a buffer that is being appended to. */
    if (b->next && b->next->free &&
        (unsigned char *)b + HDR + b->size == (unsigned char *)b->next &&
        b->size + HDR + b->next->size >= want) {
        b->size += HDR + b->next->size;
        b->next = b->next->next;
        split(b, want);
        return p;
    }
    void *q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, b->size < n ? b->size : n);
    free(p);
    return q;
}

void *aligned_alloc(size_t align, size_t n)
{
    if (align <= ALIGN)
        return malloc(n);          /* every block is already this aligned */

    /* Over-allocate, then SPLICE A REAL HEADER in front of the aligned
     * address and give the leading fragment back to the free list.
     *
     * The tempting shortcut -- return an interior pointer with the original
     * stashed below it -- makes a pointer that free() cannot read, since
     * free() finds its header at p - HDR. C requires aligned_alloc'd memory
     * to be freed with plain free(), so there must be exactly one kind of
     * block and this has to be one of them. */
    size_t want = ALIGN_UP(n);
    void *raw = malloc(want + align + HDR);
    if (!raw)
        return NULL;
    struct blk *b = (struct blk *)((unsigned char *)raw - HDR);
    size_t a = ((size_t)raw + align - 1) & ~(align - 1);
    if (a == (size_t)raw)
        return raw;
    while (a - (size_t)raw < HDR)  /* leave room for the new header */
        a += align;

    struct blk *nb = (struct blk *)(a - HDR);
    size_t lead = (size_t)((unsigned char *)nb - (unsigned char *)b);
    nb->size = b->size - lead;
    nb->free = 0;
    nb->next = b->next;
    b->size = lead - HDR;          /* the fragment before it, back to the list */
    b->free = 1;
    b->next = nb;
    return (void *)a;
}
