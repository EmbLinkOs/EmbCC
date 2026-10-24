/* The formatted-output engine's insides, shared by its two instances: the
 * byte printfs' (format.c) and the wide printfs' (wide.c). What a
 * conversion makes that is not a string argument or the format's own text
 * -- digits, signs, padding, "inf" -- is ASCII and made once, here, for
 * both; each instance has its own %c, %s and parse of the format, so a
 * program that prints bytes links nothing of the wide one.
 */
#ifndef EMBLIBC_FORMAT_H
#define EMBLIBC_FORMAT_H

#include <stdarg.h>
#include <stddef.h>
#include <wchar.h>

/* Where the characters go: a sink of bytes, or one of wide characters --
 * a character is whichever the output is made of, and widths, precisions
 * on strings and %n count those. */
struct out {
    void (*sink)(void *, const char *, size_t);
    void (*wsink)(void *, const wchar_t *, size_t);   /* or this */
    void *ctx;
    int n;                       /* characters that WOULD have been written */
    int bad;                     /* an encoding error: the call fails */
};

/* Flags, in the order §7.21.6.1 lists them. */
#define F_MINUS 0x01
#define F_PLUS  0x02
#define F_SPACE 0x04
#define F_HASH  0x08
#define F_ZERO  0x10

/* ASCII out: n bytes, or n wide characters of the same values. */
void __fmt_emit(struct out *o, const char *s, size_t n);
void __fmt_pad(struct out *o, char c, int n);

/* One integer, already made unsigned and with its sign decided. */
void __fmt_int(struct out *o, unsigned long long v, int base, int upper,
               int flags, int width, int prec, const char *sign,
               const char *prefix);
void __fmt_double(struct out *o, double v, char conv, int flags, int width,
                  int prec);
void __fmt_ldouble(struct out *o, long double v, char conv, int flags,
                   int width, int prec);

#endif
