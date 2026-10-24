/* The formatted-input engine's insides, shared by its two instances: the
 * byte scanfs' (scan.c) and the wide scanfs' (wide.c). The input is bytes
 * or wide characters, which `get` returns as their values (EOF at the
 * end); the numbers are ASCII either way and are read once, here, for
 * both. Each instance has its own parse of the format and its own %c, %s
 * and %[, so a program that scans bytes links nothing of the wide one. A
 * character is whichever the input is made of: widths and %n count those.
 */
#ifndef EMBLIBC_SCAN_H
#define EMBLIBC_SCAN_H

#include <stdarg.h>
#include <stdio.h>

/* Length modifiers, as a width in bytes plus a "this is a double family"
 * flag -- the same normalisation the printf engine does on the way out. */
#define L_CHAR   1
#define L_SHORT  2
#define L_INT    3
#define L_LONG   4
#define L_LLONG  5
#define L_INTMAX 6
#define L_SIZE   7
#define L_PTRDIFF 8
#define L_DOUBLE 9        /* 'l' before a float conversion */
#define L_LDOUBLE 10

struct scan {
    int (*get)(void *);
    void (*unget)(void *, int);
    void *ctx;
    long nread;           /* characters consumed, for %n */
    int pushed;           /* a character is waiting in `back` */
    int back;
    int hit_eof;          /* an INPUT failure happened (end of input) */
};

static inline int sgetc(struct scan *s)
{
    int c;
    if (s->pushed) { c = s->back; s->pushed = 0; }
    else c = s->get(s->ctx);
    if (c != EOF)
        s->nread++;
    else
        s->hit_eof = 1;
    return c;
}

static inline void sungetc(struct scan *s, int c)
{
    if (c == EOF)
        return;
    s->back = c;
    s->pushed = 1;
    s->nread--;
}

/* An integer in `base` (0: by its prefix, as %i), a float of any of
 * strtod's forms, a pointer as %p prints it: 1 on success, 0 on a matching
 * failure. */
int __scan_int(struct scan *s, int base, int width, int assign, va_list *ap,
               int len);
int __scan_flt(struct scan *s, int width, int assign, va_list *ap, int len);
int __scan_ptr(struct scan *s, int width, int assign, va_list *ap);
void __scan_store_int(va_list *ap, int len, unsigned long long v, int neg);

#endif
