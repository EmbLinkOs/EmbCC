/* The printf family. Every one of them is __vformat with a different sink;
 * the differences between them live here and nowhere else. */
#include "file.h"

#include <string.h>

/* The caller holds the stream's lock for the whole conversion, so this
 * uses the unlocked write. Locking per chunk instead would let two
 * threads' output interleave inside one printf, which is the failure
 * the per-stream lock exists to prevent -- and would take the lock once
 * per character besides. */
static void sink_file(void *ctx, const char *s, size_t n)
{
    __fwrite_unlocked(s, 1, n, (FILE *)ctx);
}

struct buf_sink { char *p; size_t cap, n; };

static void sink_buf(void *ctx, const char *s, size_t n)
{
    struct buf_sink *b = ctx;
    /* Truncate, but keep counting: snprintf must return the length the
     * output WOULD have had, which is what lets a caller size a buffer. */
    for (size_t i = 0; i < n; i++, b->n++)
        if (b->cap && b->n + 1 < b->cap)
            b->p[b->n] = s[i];
}

int vfprintf(FILE *restrict f, const char *restrict fmt, va_list ap)
{
    if (!f) return -1;
    __flockfile(f);
    int r = __vformat(sink_file, f, fmt, ap);
    __funlockfile(f);
    return r;
}

int vprintf(const char *restrict fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int vsnprintf(char *restrict s, size_t n, const char *restrict fmt,
              va_list ap)
{
    struct buf_sink b = { s, n, 0 };
    int r = __vformat(sink_buf, &b, fmt, ap);
    if (n) s[b.n < n - 1 ? b.n : n - 1] = 0;
    return r;
}

int vsprintf(char *restrict s, const char *restrict fmt, va_list ap)
{
    return vsnprintf(s, (size_t)-1, fmt, ap);
}

int printf(const char *restrict fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *restrict f, const char *restrict fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int snprintf(char *restrict s, size_t n, const char *restrict fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *restrict s, const char *restrict fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(s, fmt, ap);
    va_end(ap);
    return r;
}
