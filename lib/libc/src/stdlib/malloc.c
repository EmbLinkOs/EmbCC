/* malloc — first fit over an explicit free list, on a break-grown heap.
 *
 * Why this and not something cleverer: the allocator a C library ships has
 * to be correct on every target before it is fast on any, and a first-fit
 * list with coalescing is small enough to read in one sitting and to reason
 * about when a target misbehaves. It is the same judgement `src/ir` made
 * about the IR — correct and small first; the shape does not prevent a
 * better one later, because nothing outside this file knows the shape.
 *
 * That judgement stands. What did not is the FIRST version's data
 * structure, which had one list — every block, in address order — and
 * searched it from the head on every malloc. A program that allocates and
 * does not free (a compiler parsing a file is exactly that) puts every
 * live block ahead of every free one, so each malloc walked all of them:
 * quadratic, and measurably so. 2000 allocations took 0.055s, 4000 took
 * 0.199s, 8000 took 0.791s, 16000 took 3.281s — four times the work for
 * twice the blocks, every step — and 60000 did not finish inside a
 * twenty-second timeout. The host's libc does the same 60000 in 0.002s.
 *
 * So there are TWO lists over the same blocks, which is the classic shape:
 *
 *   - the ADDRESS list (`prev`, `next`), every block in address order. It
 *     exists only for coalescing, which is a question about neighbours;
 *     making it doubly linked is what lets free() join a block to the one
 *     BEFORE it without scanning from the head.
 *   - the FREE list (`fprev`, `fnext`), only free blocks, unordered.
 *     malloc walks this one, so the cost of an allocation depends on how
 *     many blocks are FREE, not on how many exist.
 *
 * The free list's links live in the PAYLOAD of free blocks, which is by
 * definition not in use, so the header does not grow to carry them: it is
 * still 32 bytes, the same as when it held one link and a flag. The flag
 * moved into the low bit of `size`, which is always a multiple of 16.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "../../os/backend.h"
#include "../internal/lock.h"

/* One lock for the whole heap.
 *
 * The block lists are global, so two threads in malloc at once splice
 * the same node twice and the free list stops being a list. It does not
 * fail there: it fails later, in an unrelated allocation, which is why
 * "malloc segfaults with four threads" was the report. Two threads was
 * enough to kill it; one was fine.
 *
 * One lock and not a per-size-class arena, for the same reason the
 * allocator is first-fit: correct on every target before fast on any.
 * The uncontended cost is a single compare-exchange (see lock.h), which
 * is what makes this acceptable on the targets that have one thread.
 * Nothing outside this file knows there is a lock, so an arena later is
 * still a local change. */
static __lock_t heap_lock = LOCK_INIT;

/* Every block is aligned for any type: max_align_t's alignment, which on
 * both targets is 16 (long double / __int128). */
#define ALIGN 16UL
#define ALIGN_UP(n) (((n) + (ALIGN - 1)) & ~(ALIGN - 1))

struct blk {
    size_t size;            /* payload bytes | FREE — see below */
    struct blk *prev;       /* address order, so coalescing is a neighbour test */
    struct blk *next;
};

/* Payload sizes are always rounded up to 16, so the low bit of `size` is
 * always zero and is free to carry the flag. aligned_alloc's spliced
 * headers keep that true: every size it computes is a difference of
 * 16-aligned addresses. */
#define FREE_BIT   1UL
#define BSIZE(b)   ((b)->size & ~FREE_BIT)
#define BFREE(b)   ((int)((b)->size & FREE_BIT))
#define SET_BLK(b, n, f) ((b)->size = (size_t)(n) | ((f) ? FREE_BIT : 0))

#define HDR ALIGN_UP(sizeof(struct blk))

/* The free-list links, in the payload. A block's payload is at least
 * ALIGN (16) bytes — malloc(0) is rounded to 1 and then up to 16, and
 * split() refuses to leave a remainder smaller than that — so there is
 * always room for two pointers. */
struct fnode { struct blk *fprev, *fnext; };
#define FN(b) ((struct fnode *)((unsigned char *)(b) + HDR))

static struct blk *g_head;     /* address order: first block */
static struct blk *g_tail;     /* ... and last, so grow() does not scan */
static struct blk *g_free;     /* the free list: unordered */

static void flist_push(struct blk *b)
{
    FN(b)->fprev = NULL;
    FN(b)->fnext = g_free;
    if (g_free)
        FN(g_free)->fprev = b;
    g_free = b;
}

static void flist_remove(struct blk *b)
{
    struct blk *pv = FN(b)->fprev, *nx = FN(b)->fnext;
    if (pv)
        FN(pv)->fnext = nx;
    else
        g_free = nx;
    if (nx)
        FN(nx)->fprev = pv;
}

/* Is `b` immediately followed in memory by `n`? Two blocks can be
 * adjacent in the address list and still have a gap, because grow() may
 * get a break extension that does not abut the previous one. */
static int abuts(struct blk *b, struct blk *n)
{
    return n && (unsigned char *)b + HDR + BSIZE(b) == (unsigned char *)n;
}

/* Ask the OS for at least `need` more bytes, in chunks so that a program
 * doing many small allocations does not make one syscall each. The block
 * comes back FREE and on the free list, like any other free block. */
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
    SET_BLK(b, chunk - HDR, 1);
    b->prev = g_tail;
    b->next = NULL;
    if (g_tail)
        g_tail->next = b;
    else
        g_head = b;
    g_tail = b;
    /* The new chunk may abut the last block: join them rather than leave
     * a seam no later coalesce can cross. */
    if (b->prev && BFREE(b->prev) && abuts(b->prev, b)) {
        struct blk *t = b->prev;
        SET_BLK(t, BSIZE(t) + HDR + BSIZE(b), 1);
        t->next = b->next;          /* NULL: b was the tail */
        g_tail = t;
        return t;                   /* already on the free list */
    }
    flist_push(b);
    return b;
}

/* Cut `want` bytes off the front of `b`, which is NOT on the free list.
 * The remainder becomes a free block of its own. */
static void split(struct blk *b, size_t want)
{
    /* Only worth splitting if the remainder can hold a header and enough
     * payload to be handed out; otherwise the tail is left as slack. */
    if (BSIZE(b) < want + HDR + ALIGN)
        return;
    struct blk *rest = (struct blk *)((unsigned char *)b + HDR + want);
    SET_BLK(rest, BSIZE(b) - want - HDR, 1);
    rest->prev = b;
    rest->next = b->next;
    if (b->next)
        b->next->prev = rest;
    else
        g_tail = rest;
    b->next = rest;
    SET_BLK(b, want, BFREE(b));
    flist_push(rest);
}

/* The body of malloc, with the heap lock already held. The public
 * entry points below take the lock once and call these, because they
 * call each other -- realloc allocates and frees, aligned_alloc
 * allocates and then splices -- and a lock taken twice by one thread is
 * a deadlock. Keeping the recursion inside and the lock outside is the
 * simplest arrangement that cannot do that. */
static void *do_malloc(size_t n)
{
    if (n == 0)
        n = 1;                 /* a unique address, as C allows and callers expect */
    size_t want = ALIGN_UP(n);
    if (want < n) {            /* the rounding overflowed: no such allocation */
        errno = ENOMEM;
        return NULL;
    }
    /* Only free blocks are walked. This is the whole fix: the list a
     * long-running allocating program grows is the list of LIVE blocks,
     * and that list is not this one. */
    for (struct blk *b = g_free; b; b = FN(b)->fnext)
        if (BSIZE(b) >= want) {
            flist_remove(b);
            SET_BLK(b, BSIZE(b), 0);
            split(b, want);
            return (unsigned char *)b + HDR;
        }
    struct blk *b = grow(want);
    if (!b)
        return NULL;
    flist_remove(b);
    SET_BLK(b, BSIZE(b), 0);
    split(b, want);
    return (unsigned char *)b + HDR;
}

static void do_free(void *p)
{
    if (!p)
        return;                /* free(NULL) is defined and does nothing */
    struct blk *b = (struct blk *)((unsigned char *)p - HDR);
    SET_BLK(b, BSIZE(b), 1);
    flist_push(b);
    /* Coalesce forward, then backward. Both are O(1) now: the address
     * list is doubly linked, so the block BEFORE this one is `b->prev`
     * rather than the end of a scan from the head. */
    struct blk *nx = b->next;
    if (nx && BFREE(nx) && abuts(b, nx)) {
        flist_remove(nx);
        SET_BLK(b, BSIZE(b) + HDR + BSIZE(nx), 1);
        b->next = nx->next;
        if (nx->next)
            nx->next->prev = b;
        else
            g_tail = b;
    }
    struct blk *pv = b->prev;
    if (pv && BFREE(pv) && abuts(pv, b)) {
        flist_remove(b);
        SET_BLK(pv, BSIZE(pv) + HDR + BSIZE(b), 1);
        pv->next = b->next;
        if (b->next)
            b->next->prev = pv;
        else
            g_tail = pv;
    }
}

void *malloc(size_t n)
{
    __lock(&heap_lock);
    void *p = do_malloc(n);
    __unlock(&heap_lock);
    return p;
}

void free(void *p)
{
    __lock(&heap_lock);
    do_free(p);
    __unlock(&heap_lock);
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

static void *do_realloc(void *p, size_t n)
{
    if (!p)
        return do_malloc(n);
    if (n == 0) { do_free(p); return NULL; }
    struct blk *b = (struct blk *)((unsigned char *)p - HDR);
    size_t want = ALIGN_UP(n);
    if (want < n) {
        errno = ENOMEM;
        return NULL;
    }
    if (BSIZE(b) >= want) {
        split(b, want);
        return p;
    }
    /* Grow in place when the next block is free and adjacent — which is the
     * common case for a buffer that is being appended to. */
    struct blk *nx = b->next;
    if (nx && BFREE(nx) && abuts(b, nx) &&
        BSIZE(b) + HDR + BSIZE(nx) >= want) {
        flist_remove(nx);
        SET_BLK(b, BSIZE(b) + HDR + BSIZE(nx), 0);
        b->next = nx->next;
        if (nx->next)
            nx->next->prev = b;
        else
            g_tail = b;
        split(b, want);
        return p;
    }
    size_t had = BSIZE(b);
    void *q = do_malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, had < n ? had : n);
    do_free(p);
    return q;
}

void *realloc(void *p, size_t n)
{
    __lock(&heap_lock);
    void *q = do_realloc(p, n);
    __unlock(&heap_lock);
    return q;
}

static void *do_aligned_alloc(size_t align, size_t n)
{
    if (align <= ALIGN)
        return do_malloc(n);       /* every block is already this aligned */

    /* Over-allocate, then SPLICE A REAL HEADER in front of the aligned
     * address and give the leading fragment back to the free list.
     *
     * The tempting shortcut -- return an interior pointer with the original
     * stashed below it -- makes a pointer that free() cannot read, since
     * free() finds its header at p - HDR. C requires aligned_alloc'd memory
     * to be freed with plain free(), so there must be exactly one kind of
     * block and this has to be one of them. */
    if (align & (align - 1)) {     /* not a power of two: no such alignment */
        errno = EINVAL;
        return NULL;
    }
    size_t want = ALIGN_UP(n);
    void *raw = do_malloc(want + align + HDR);
    if (!raw)
        return NULL;
    struct blk *b = (struct blk *)((unsigned char *)raw - HDR);
    size_t a = ((size_t)raw + align - 1) & ~(align - 1);
    if (a == (size_t)raw)
        return raw;
    /* The leading fragment must be a REAL block, not just a gap: room
     * for its header AND for a payload of at least ALIGN.
     *
     * `HDR` alone was the old condition, and it left a fragment with a
     * zero-byte payload whenever the aligned address landed exactly HDR
     * past `raw` -- reachable for any align of 64 or more. That was
     * survivable when a free block held nothing in its payload. It is
     * not now: the free-list links live there, so pushing a zero-payload
     * block wrote sixteen bytes straight through the next block's
     * header. tests/golden/malloc.sh found it as "aligned_alloc block
     * damaged".
     *
     * `a - raw` is already a multiple of ALIGN (raw is ALIGN-aligned and
     * `align` is a larger power of two), so the fragment's payload is a
     * whole number of ALIGN units, which the FREE bit in `size` also
     * depends on. The over-allocation above covers the extra step: the
     * worst case is `align + HDR`, and malloc was asked for
     * `want + align + HDR`. */
    while (a - (size_t)raw < HDR + ALIGN)
        a += align;

    struct blk *nb = (struct blk *)(a - HDR);
    size_t lead = (size_t)((unsigned char *)nb - (unsigned char *)b);
    SET_BLK(nb, BSIZE(b) - lead, 0);
    nb->prev = b;
    nb->next = b->next;
    if (b->next)
        b->next->prev = nb;
    else
        g_tail = nb;
    b->next = nb;
    SET_BLK(b, lead - HDR, 1);     /* the fragment before it, back to the list */
    flist_push(b);
    return (void *)a;
}

void *aligned_alloc(size_t align, size_t n)
{
    __lock(&heap_lock);
    void *p = do_aligned_alloc(align, n);
    __unlock(&heap_lock);
    return p;
}
