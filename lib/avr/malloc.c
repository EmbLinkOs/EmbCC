/* The AVR library's heap (lib/avr): malloc, free, calloc and realloc, as
 * avr-libc lays the heap out, so a program written for it -- or C++'s
 * operator new -- finds what it expects.
 *
 * The heap starts at __malloc_heap_start (the end of .bss and .noinit,
 * __heap_start in the part's linker script) and grows up towards the
 * stack. Its end is __malloc_heap_end when a program sets it, and
 * otherwise the stack pointer less __malloc_margin bytes, read at each
 * call: on a 2 KB part the stack and the heap share what is left.
 *
 * A block is a size word and its bytes; a free one keeps the next free
 * block's address in its first two, so nothing smaller than two bytes is
 * handed out. The free list is in address order, first fit, a block
 * split when what is left can hold another, neighbours joined on free,
 * and the last block given back to the break -- avr-libc's own policy. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct __emb_avr_fb {
    size_t sz;                      /* the bytes after this word */
    struct __emb_avr_fb *next;      /* (a free block's first two) */
};

extern char *__malloc_heap_start, *__malloc_heap_end;
extern size_t __malloc_margin;
extern char *__brkval;
extern struct __emb_avr_fb *__flp;
void *malloc(size_t n);
void free(void *p);

#ifdef EMB_AVR_MALLOC
extern char __heap_start;
char *__malloc_heap_start = &__heap_start;
char *__malloc_heap_end;
size_t __malloc_margin = 32;
char *__brkval;                     /* the break: the heap's top so far */
struct __emb_avr_fb *__flp;         /* the free list, by address */

#define SPL_SPH (*(volatile uint16_t *)0x5d)

void *malloc(size_t n)
{
    if (n < sizeof(struct __emb_avr_fb *))
        n = sizeof(struct __emb_avr_fb *);
    /* first fit */
    struct __emb_avr_fb **pp = &__flp;
    for (struct __emb_avr_fb *b = __flp; b; pp = &b->next, b = b->next) {
        if (b->sz < n)
            continue;
        if (b->sz >= n + sizeof(struct __emb_avr_fb)) {
            /* split: the tail stays free, in this block's place */
            struct __emb_avr_fb *t =
                (struct __emb_avr_fb *)((char *)&b->next + n);
            t->sz = b->sz - n - sizeof(size_t);
            t->next = b->next;
            b->sz = n;
            *pp = t;
        } else {
            *pp = b->next;
        }
        return &b->next;
    }
    /* none: from the break, below the stack's margin */
    if (!__brkval)
        __brkval = __malloc_heap_start;
    char *top = __malloc_heap_end ? __malloc_heap_end
                                  : (char *)SPL_SPH - __malloc_margin;
    if (__brkval >= top || (size_t)(top - __brkval) < n + sizeof(size_t))
        return NULL;
    struct __emb_avr_fb *b = (struct __emb_avr_fb *)__brkval;
    b->sz = n;
    __brkval += n + sizeof(size_t);
    return &b->next;
}

void free(void *p)
{
    if (!p)
        return;
    struct __emb_avr_fb *b =
        (struct __emb_avr_fb *)((char *)p - sizeof(size_t));
    /* the last block goes straight back to the break */
    if ((char *)p + b->sz == __brkval) {
        __brkval = (char *)b;
        /* ...and so does a free block that now ends there */
        struct __emb_avr_fb **pp = &__flp;
        while (*pp && (*pp)->next)
            pp = &(*pp)->next;
        if (*pp && (char *)&(*pp)->next + (*pp)->sz == __brkval) {
            __brkval = (char *)*pp;
            *pp = NULL;
        }
        return;
    }
    /* into the list in address order, joined with what touches it */
    struct __emb_avr_fb *prev = NULL, *q = __flp;
    while (q && q < b) {
        prev = q;
        q = q->next;
    }
    b->next = q;
    if (q && (char *)&b->next + b->sz == (char *)q) {
        b->sz += sizeof(size_t) + q->sz;
        b->next = q->next;
    }
    if (prev && (char *)&prev->next + prev->sz == (char *)b) {
        prev->sz += sizeof(size_t) + b->sz;
        prev->next = b->next;
    } else if (prev) {
        prev->next = b;
    } else {
        __flp = b;
    }
}
#endif

#ifdef EMB_AVR_CALLOC
void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    void *p = malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}
#endif

#ifdef EMB_AVR_REALLOC
void *realloc(void *p, size_t n)
{
    if (!p)
        return malloc(n);
    size_t had = ((size_t *)p)[-1];
    if (n <= had)
        return p;
    void *q = malloc(n);
    if (q) {
        memcpy(q, p, had);
        free(p);
    }
    return q;
}
#endif

#ifdef EMB_AVR_ALIGNED_ALLOC
/* nothing on AVR needs more than a byte's alignment, but an over-aligned
 * operator new asks: a block big enough to start an aligned one in, its
 * leading part handed back to the free list as a block of its own */
void *aligned_alloc(size_t align, size_t n)
{
    if (align <= sizeof(size_t))
        return malloc(n);
    if (n < sizeof(struct __emb_avr_fb *))      /* as malloc's least */
        n = sizeof(struct __emb_avr_fb *);
    char *raw = malloc(n + align + sizeof(struct __emb_avr_fb));
    if (!raw)
        return NULL;
    uintptr_t a = ((uintptr_t)raw + sizeof(struct __emb_avr_fb) + align - 1) &
                  ~(uintptr_t)(align - 1);
    struct __emb_avr_fb *head = (struct __emb_avr_fb *)(raw - sizeof(size_t));
    size_t total = head->sz, lead = (size_t)((char *)a - raw);
    struct __emb_avr_fb *nb =
        (struct __emb_avr_fb *)((char *)a - sizeof(size_t));
    nb->sz = total - lead;
    head->sz = lead - sizeof(size_t);
    free(raw);
    return (void *)a;
}
#endif

#ifdef EMB_AVR_ABORT
/* abort: exit with SIGABRT's status, as a C++ terminate wants; the
 * startup's exit stops the part (.fini0) */
extern void _exit(int status) __attribute__((noreturn));
void abort(void)
{
    _exit(134);
}
#endif
