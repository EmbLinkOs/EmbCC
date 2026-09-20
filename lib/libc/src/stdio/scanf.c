/* The public scanf family: six functions, two sources, one engine.
 *
 * The FILE source is fgetc/ungetc, which means a scanf that stops early
 * leaves the stream exactly where the standard says -- the pushback is the
 * SAME one-character slot a caller's own ungetc uses, so the two cannot
 * disagree about where the file is.
 */
#include "file.h"

#include <stdarg.h>
#include <stdio.h>

static int get_file(void *ctx) { return fgetc((FILE *)ctx); }
static void unget_file(void *ctx, int c) { ungetc(c, (FILE *)ctx); }

/* A string behaves as a stream that ends at its NUL. The cursor may step
 * back exactly once, which is all the engine asks for. */
struct strsrc { const unsigned char *p; size_t i; };

static int get_str(void *ctx)
{
    struct strsrc *s = ctx;
    return s->p[s->i] ? (int)s->p[s->i++] : EOF;
}

static void unget_str(void *ctx, int c)
{
    struct strsrc *s = ctx;
    if (c != EOF && s->i)
        s->i--;
}

int vfscanf(FILE *restrict f, const char *restrict fmt, va_list ap)
{
    return __vscan(get_file, unget_file, f, fmt, ap);
}

int vscanf(const char *restrict fmt, va_list ap)
{
    return vfscanf(stdin, fmt, ap);
}

int vsscanf(const char *restrict s, const char *restrict fmt, va_list ap)
{
    struct strsrc src;
    src.p = (const unsigned char *)s;
    src.i = 0;
    return __vscan(get_str, unget_str, &src, fmt, ap);
}

int fscanf(FILE *restrict f, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(f, fmt, ap);
    va_end(ap);
    return n;
}

int scanf(const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return n;
}

int sscanf(const char *restrict s, const char *restrict fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsscanf(s, fmt, ap);
    va_end(ap);
    return n;
}
