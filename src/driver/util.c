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

/* ---- the options as the user typed them ----------------------------------
 *
 * The driver rewrites EmbCC's option spellings into the ones its argument
 * loop reads (src/driver/options.c). A message about an option quotes
 * what the user typed: opt_typed("-fshort-enums") is "--enums=short" when
 * that is how it came, and "-fshort-enums" when that is. Here rather than
 * in options.c so that every part of the compiler, and every tool that
 * links a part of it, can ask. */
static struct { char *internal, *typed; } *g_opt_typed;
static int g_nopt_typed, g_capopt_typed;

void opt_remember_typed(const char *internal, const char *typed)
{
    if (g_nopt_typed == g_capopt_typed) {
        g_capopt_typed = g_capopt_typed ? 2 * g_capopt_typed : 8;
        g_opt_typed = xrealloc(g_opt_typed,
                               (size_t)g_capopt_typed * sizeof *g_opt_typed);
    }
    char *in = xmalloc(strlen(internal) + 1), *ty = xmalloc(strlen(typed) + 1);
    strcpy(in, internal);
    strcpy(ty, typed);
    g_opt_typed[g_nopt_typed].internal = in;
    g_opt_typed[g_nopt_typed].typed = ty;
    g_nopt_typed++;
}

const char *opt_typed(const char *internal)
{
    for (int k = 0; k < g_nopt_typed; k++)
        if (strcmp(g_opt_typed[k].internal, internal) == 0)
            return g_opt_typed[k].typed;
    return internal;
}

/* The strings opt_said returns: a message may name a few options at
 * once, and each must outlive the next call. */
static const char *opt_ring(char *s)
{
    static char *ring[16];
    static int at;
    free(ring[at]);
    ring[at] = s;
    at = (at + 1) % 16;
    return s;
}

static char *opt_join(const char *prefix, const char *value)
{
    size_t n = strlen(prefix) + (value ? strlen(value) : 0) + 1;
    char *s = xmalloc(n);
    strcpy(s, prefix);
    if (value)
        strcat(s, value);
    return s;
}

/* The option PREFIX VALUE as the user typed it: opt_said("-mfpu=",
 * "fpv5-d16") is "--fpu=fpv5-d16" when that is how it came. */
const char *opt_said(const char *prefix, const char *value)
{
    char *s = opt_join(prefix, value);
    const char *t = opt_typed(s);
    if (t != s) {
        free(s);
        return t;
    }
    return opt_ring(s);
}
