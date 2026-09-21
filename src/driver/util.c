#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The one place in a library that still ends the process (R6's exception):
 * under out-of-memory, building and rendering a diagnostic would itself
 * allocate, so unwinding to the boundary is the less reliable choice, not
 * the more. Everything else reports and unwinds -- see fatal_unwind(). */
void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "embcc: out of memory\n");
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fprintf(stderr, "embcc: out of memory\n");
        exit(1);
    }
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "embcc: out of memory\n");
        exit(1);
    }
    return p;
}

/* strndup is POSIX, not C99, and the source must stay strict C99
 * (ARCHITECTURE §7) — so EmbCC carries its own. */
char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ---- the growable text buffer (util.h) ---- */

void ob_add(struct outbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t want = b->cap ? b->cap : 256;
        while (want < b->n + n + 1)
            want *= 2;
        b->p = xrealloc(b->p, want);
        b->cap = want;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void ob_str(struct outbuf *b, const char *s) { ob_add(b, s, strlen(s)); }
void ob_ch(struct outbuf *b, char c) { ob_add(b, &c, 1); }

int ob_fmt(struct outbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0)
        return 0;
    if ((size_t)n < sizeof tmp) {
        ob_add(b, tmp, (size_t)n);
        return n;
    }
    char *big = xmalloc((size_t)n + 1);   /* rare: a very long path list */
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    ob_add(b, big, (size_t)n);
    free(big);
    return n;
}

void ob_free(struct outbuf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}
