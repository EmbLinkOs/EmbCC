/* <obstack.h> -- GNU's object stacks (src/posix/obstack.c): objects
 * grown in place a byte or a block at a time, finished, and freed back to
 * any earlier one, from chunks the caller's allocator gives.
 *
 * glibc's are macros; these are inline functions where a function can do
 * it (each argument evaluated once), macros only for what must be one --
 * the lvalues (obstack_alignment_mask(h) = 0), and the calls that name
 * the program's obstack_chunk_alloc and obstack_chunk_free. The counts
 * are size_t, as gnulib's are (glibc keeps int for its old ABI). */
#ifndef _OBSTACK_H
#define _OBSTACK_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _obstack_chunk {
    char *limit;                       /* one past the chunk's end */
    struct _obstack_chunk *prev;       /* the chunk before, or null */
    char contents[4];
};

struct obstack {
    size_t chunk_size;                 /* a new chunk's size at least */
    struct _obstack_chunk *chunk;      /* the current chunk */
    char *object_base;                 /* where the growing object starts */
    char *next_free;                   /* where it ends */
    char *chunk_limit;                 /* the current chunk's end */
    union { size_t i; void *p; } temp;
    size_t alignment_mask;             /* a finished object's alignment - 1 */
    union {
        void *(*plain)(size_t);
        void *(*extra)(void *, size_t);
    } chunkfun;
    union {
        void (*plain)(void *);
        void (*extra)(void *, void *);
    } freefun;
    void *extra_arg;                   /* the first argument, with use_extra_arg */
    unsigned use_extra_arg : 1;
    unsigned maybe_empty_object : 1;
    unsigned alloc_failed : 1;         /* never set: kept for glibc's sources */
};

int  _obstack_begin(struct obstack *h, size_t size, size_t alignment,
                    void *(*chunkfun)(size_t), void (*freefun)(void *));
int  _obstack_begin_1(struct obstack *h, size_t size, size_t alignment,
                      void *(*chunkfun)(void *, size_t),
                      void (*freefun)(void *, void *), void *arg);
void _obstack_newchunk(struct obstack *h, size_t length);
void _obstack_free(struct obstack *h, void *obj);
void obstack_free(struct obstack *h, void *obj);
size_t _obstack_memory_used(struct obstack *h);
int  _obstack_allocated_p(struct obstack *h, void *obj);

/* called when a chunk cannot be had; it must not return. The default
 * says so on stderr and exits with obstack_exit_failure. */
extern void (*obstack_alloc_failed_handler)(void);
extern int obstack_exit_failure;

int obstack_printf(struct obstack *h, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int obstack_vprintf(struct obstack *h, const char *fmt, va_list ap)
    __attribute__((format(printf, 2, 0)));

#define obstack_init(h) \
    _obstack_begin((h), 0, 0, (void *(*)(size_t))obstack_chunk_alloc, \
                   (void (*)(void *))obstack_chunk_free)
#define obstack_begin(h, size) \
    _obstack_begin((h), (size), 0, (void *(*)(size_t))obstack_chunk_alloc, \
                   (void (*)(void *))obstack_chunk_free)
#define obstack_specify_allocation(h, size, alignment, chunkfun, freefun) \
    _obstack_begin((h), (size), (alignment), (void *(*)(size_t))(chunkfun), \
                   (void (*)(void *))(freefun))
#define obstack_specify_allocation_with_arg(h, size, alignment, chunkfun, freefun, arg) \
    _obstack_begin_1((h), (size), (alignment), \
                     (void *(*)(void *, size_t))(chunkfun), \
                     (void (*)(void *, void *))(freefun), (arg))
#define obstack_chunkfun(h, fn) ((void)((h)->chunkfun.extra = (void *(*)(void *, size_t))(fn)))
#define obstack_freefun(h, fn)  ((void)((h)->freefun.extra = (void (*)(void *, void *))(fn)))

#define obstack_base(h)           ((void *)(h)->object_base)
#define obstack_next_free(h)      ((void *)(h)->next_free)
#define obstack_chunk_size(h)     ((h)->chunk_size)
#define obstack_alignment_mask(h) ((h)->alignment_mask)
#define obstack_memory_used(h)    _obstack_memory_used(h)
#define obstack_1grow_fast(h, c)  ((void)(*((h)->next_free)++ = (char)(c)))
#define obstack_blank_fast(h, n)  ((void)((h)->next_free += (n)))

static inline size_t obstack_object_size(struct obstack *h)
{
    return (size_t)(h->next_free - h->object_base);
}

static inline size_t obstack_room(struct obstack *h)
{
    return (size_t)(h->chunk_limit - h->next_free);
}

static inline char *__obstack_align(char *p, size_t mask)
{
    return p + ((((uintptr_t)p + mask) & ~(uintptr_t)mask) - (uintptr_t)p);
}

static inline int obstack_empty_p(struct obstack *h)
{
    return h->chunk->prev == 0 &&
           h->next_free == __obstack_align(h->chunk->contents, h->alignment_mask);
}

static inline void obstack_make_room(struct obstack *h, size_t n)
{
    if (obstack_room(h) < n)
        _obstack_newchunk(h, n);
}

static inline void obstack_grow(struct obstack *h, const void *p, size_t n)
{
    obstack_make_room(h, n);
    if (n)
        memcpy(h->next_free, p, n);
    h->next_free += n;
}

static inline void obstack_grow0(struct obstack *h, const void *p, size_t n)
{
    obstack_make_room(h, n + 1);
    if (n)
        memcpy(h->next_free, p, n);
    h->next_free += n;
    *h->next_free++ = 0;
}

static inline void obstack_1grow(struct obstack *h, int c)
{
    obstack_make_room(h, 1);
    *h->next_free++ = (char)c;
}

static inline void obstack_ptr_grow_fast(struct obstack *h, const void *p)
{
    memcpy(h->next_free, &p, sizeof p);
    h->next_free += sizeof p;
}

static inline void obstack_ptr_grow(struct obstack *h, const void *p)
{
    obstack_make_room(h, sizeof p);
    obstack_ptr_grow_fast(h, p);
}

static inline void obstack_int_grow_fast(struct obstack *h, int v)
{
    memcpy(h->next_free, &v, sizeof v);
    h->next_free += sizeof v;
}

static inline void obstack_int_grow(struct obstack *h, int v)
{
    obstack_make_room(h, sizeof v);
    obstack_int_grow_fast(h, v);
}

static inline void obstack_blank(struct obstack *h, size_t n)
{
    obstack_make_room(h, n);
    h->next_free += n;
}

/* the object, done: the next one starts after it, aligned */
static inline void *obstack_finish(struct obstack *h)
{
    void *v = h->object_base;
    if (h->next_free == h->object_base)
        h->maybe_empty_object = 1;
    h->next_free = __obstack_align(h->next_free, h->alignment_mask);
    if ((size_t)(h->next_free - (char *)h->chunk) >
        (size_t)(h->chunk_limit - (char *)h->chunk))
        h->next_free = h->chunk_limit;
    h->object_base = h->next_free;
    return v;
}

static inline void *obstack_alloc(struct obstack *h, size_t n)
{
    obstack_blank(h, n);
    return obstack_finish(h);
}

static inline void *obstack_copy(struct obstack *h, const void *p, size_t n)
{
    obstack_grow(h, p, n);
    return obstack_finish(h);
}

static inline void *obstack_copy0(struct obstack *h, const void *p, size_t n)
{
    obstack_grow0(h, p, n);
    return obstack_finish(h);
}

#ifdef __cplusplus
}
#endif

#endif
