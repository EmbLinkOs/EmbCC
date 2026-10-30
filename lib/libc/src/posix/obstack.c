/* GNU's object stacks: the out-of-line half of <obstack.h>. The chunk
 * sizes and the growth rule are glibc's (a default chunk of 4064 bytes,
 * a new one an eighth bigger than the object plus 100), and so is the
 * rule that a chunk which held only the moving object is freed. */
#define _GNU_SOURCE 1
#include <obstack.h>
#include <stdio.h>
#include <stdlib.h>

#include "posix.h"

union fullalign { uintmax_t i; long double d; void *p; };
#define DEFAULT_ALIGN offsetof(struct { char c; union fullalign u; }, u)
#define DEFAULT_ROUND sizeof(union fullalign)

static void print_and_abort(void)
{
    fputs("memory exhausted\n", stderr);
    exit(obstack_exit_failure);
}

WEAK void (*obstack_alloc_failed_handler)(void) = print_and_abort;
WEAK int obstack_exit_failure = EXIT_FAILURE;

static void *call_chunkfun(struct obstack *h, size_t n)
{
    return h->use_extra_arg ? h->chunkfun.extra(h->extra_arg, n) : h->chunkfun.plain(n);
}

static void call_freefun(struct obstack *h, void *p)
{
    if (h->use_extra_arg)
        h->freefun.extra(h->extra_arg, p);
    else
        h->freefun.plain(p);
}

static int begin(struct obstack *h, size_t size, size_t alignment)
{
    if (!alignment)
        alignment = DEFAULT_ALIGN;
    if (!size) {
        size_t extra = (((12 + DEFAULT_ROUND - 1) & ~(DEFAULT_ROUND - 1)) + 4 +
                        DEFAULT_ROUND - 1) & ~(DEFAULT_ROUND - 1);
        size = 4096 - extra;
    }
    h->chunk_size = size;
    h->alignment_mask = alignment - 1;
    struct _obstack_chunk *c = call_chunkfun(h, size);
    if (!c)
        obstack_alloc_failed_handler();
    h->chunk = c;
    h->next_free = h->object_base = __obstack_align(c->contents, h->alignment_mask);
    h->chunk_limit = c->limit = (char *)c + size;
    c->prev = 0;
    h->maybe_empty_object = 0;
    h->alloc_failed = 0;
    return 1;
}

WEAK int _obstack_begin(struct obstack *h, size_t size, size_t alignment,
                        void *(*chunkfun)(size_t), void (*freefun)(void *))
{
    h->chunkfun.plain = chunkfun;
    h->freefun.plain = freefun;
    h->use_extra_arg = 0;
    return begin(h, size, alignment);
}

WEAK int _obstack_begin_1(struct obstack *h, size_t size, size_t alignment,
                          void *(*chunkfun)(void *, size_t),
                          void (*freefun)(void *, void *), void *arg)
{
    h->chunkfun.extra = chunkfun;
    h->freefun.extra = freefun;
    h->extra_arg = arg;
    h->use_extra_arg = 1;
    return begin(h, size, alignment);
}

/* a chunk with room for the growing object and length more; the object
 * moves there */
WEAK void _obstack_newchunk(struct obstack *h, size_t length)
{
    struct _obstack_chunk *old = h->chunk, *c = 0;
    size_t obj = (size_t)(h->next_free - h->object_base);
    size_t sum1 = obj + length, sum2 = sum1 + h->alignment_mask;
    size_t size = sum2 + (obj >> 3) + 100;
    if (size < sum2)
        size = sum2;
    if (size < h->chunk_size)
        size = h->chunk_size;
    if (obj <= sum1 && sum1 <= sum2)
        c = call_chunkfun(h, size);
    if (!c)
        obstack_alloc_failed_handler();
    h->chunk = c;
    c->prev = old;
    c->limit = h->chunk_limit = (char *)c + size;
    char *base = __obstack_align(c->contents, h->alignment_mask);
    memcpy(base, h->object_base, obj);
    /* the old chunk held nothing but this object: give it back */
    if (!h->maybe_empty_object &&
        h->object_base == __obstack_align(old->contents, h->alignment_mask)) {
        c->prev = old->prev;
        call_freefun(h, old);
    }
    h->object_base = base;
    h->next_free = base + obj;
    h->maybe_empty_object = 0;
}

WEAK int _obstack_allocated_p(struct obstack *h, void *obj)
{
    struct _obstack_chunk *c = h->chunk;
    while (c && ((void *)c >= obj || (void *)c->limit < obj))
        c = c->prev;
    return c != 0;
}

/* everything allocated after obj, and obj, goes; a null obj frees all */
WEAK void _obstack_free(struct obstack *h, void *obj)
{
    struct _obstack_chunk *c = h->chunk;
    while (c && ((void *)c >= obj || (void *)c->limit < obj)) {
        struct _obstack_chunk *prev = c->prev;
        call_freefun(h, c);
        c = prev;
        h->maybe_empty_object = 1;
    }
    if (c) {
        h->object_base = h->next_free = obj;
        h->chunk_limit = c->limit;
        h->chunk = c;
    } else if (obj) {
        abort();
    }
}

WEAK void obstack_free(struct obstack *h, void *obj)
{
    _obstack_free(h, obj);
}

WEAK size_t _obstack_memory_used(struct obstack *h)
{
    size_t n = 0;
    for (struct _obstack_chunk *c = h->chunk; c; c = c->prev)
        n += (size_t)(c->limit - (char *)c);
    return n;
}

/* the formatted text grows the object, without a NUL */
WEAK int obstack_vprintf(struct obstack *h, const char *fmt, va_list ap)
{
    va_list again;
    va_copy(again, ap);
    size_t room = obstack_room(h);
    int n = vsnprintf(h->next_free, room, fmt, ap);
    if (n >= 0 && (size_t)n >= room) {
        obstack_make_room(h, (size_t)n + 1);
        n = vsnprintf(h->next_free, (size_t)n + 1, fmt, again);
    }
    va_end(again);
    if (n > 0)
        h->next_free += n;
    return n;
}

WEAK int obstack_printf(struct obstack *h, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = obstack_vprintf(h, fmt, ap);
    va_end(ap);
    return n;
}
