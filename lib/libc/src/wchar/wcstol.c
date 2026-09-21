/* The wide numeric conversions, C11 §7.29.4.1.
 *
 * Each converts its input to bytes and hands it to the narrow
 * function, and that is correct rather than lazy: every character a
 * number can contain -- digits, signs, points, exponents, `0x`, `inf`,
 * `nan` -- is in the basic execution set, where a wchar_t below 128 IS
 * its byte. Anything outside that range cannot be part of a number, so
 * it ends the conversion, and the end pointer maps back by COUNTING
 * characters rather than bytes.
 *
 * Reimplementing strtod for wide input would mean a second copy of the
 * exact decimal arithmetic in lib/libc/src/stdlib/strtod.c, and a
 * second copy is a second thing to get wrong.
 */
#include <wchar.h>
#include <stdlib.h>

/* A number longer than this has extra digits that cannot change the
 * answer -- see strtod.c on why digits past the accumulator's width
 * only shift the exponent. */
#define MAXNUM 512

static void narrow(const wchar_t *s, char *buf)
{
    size_t i = 0;
    while (i + 1 < MAXNUM && s[i] && (unsigned)s[i] < 128u) {
        buf[i] = (char)s[i];
        i++;
    }
    buf[i] = 0;
}

/* The end pointer, mapped from a byte offset back to a character
 * offset. They are the same number, because every byte that was copied
 * came from exactly one wchar_t below 128. */
static void put_end(const wchar_t *s, wchar_t **end, const char *buf,
                    const char *e)
{
    if (end)
        *end = (wchar_t *)s + (e ? (size_t)(e - buf) : 0);
}

long wcstol(const wchar_t *__restrict s, wchar_t **__restrict end, int base)
{
    char buf[MAXNUM], *e = NULL;
    narrow(s, buf);
    long v = strtol(buf, &e, base);
    put_end(s, end, buf, e);
    return v;
}

unsigned long wcstoul(const wchar_t *__restrict s, wchar_t **__restrict end,
                      int base)
{
    char buf[MAXNUM], *e = NULL;
    narrow(s, buf);
    unsigned long v = strtoul(buf, &e, base);
    put_end(s, end, buf, e);
    return v;
}

long long wcstoll(const wchar_t *__restrict s, wchar_t **__restrict end,
                  int base)
{
    char buf[MAXNUM], *e = NULL;
    narrow(s, buf);
    long long v = strtoll(buf, &e, base);
    put_end(s, end, buf, e);
    return v;
}

unsigned long long wcstoull(const wchar_t *__restrict s,
                            wchar_t **__restrict end, int base)
{
    char buf[MAXNUM], *e = NULL;
    narrow(s, buf);
    unsigned long long v = strtoull(buf, &e, base);
    put_end(s, end, buf, e);
    return v;
}

double wcstod(const wchar_t *__restrict s, wchar_t **__restrict end)
{
    char buf[MAXNUM], *e = NULL;
    narrow(s, buf);
    double v = strtod(buf, &e);
    put_end(s, end, buf, e);
    return v;
}

float wcstof(const wchar_t *__restrict s, wchar_t **__restrict end)
{
    char buf[MAXNUM], *e = NULL;
    narrow(s, buf);
    float v = strtof(buf, &e);
    put_end(s, end, buf, e);
    return v;
}

long double wcstold(const wchar_t *__restrict s, wchar_t **__restrict end)
{
    char buf[MAXNUM], *e = NULL;
    narrow(s, buf);
    long double v = strtold(buf, &e);
    put_end(s, end, buf, e);
    return v;
}
