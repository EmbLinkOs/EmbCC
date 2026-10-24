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
 *
 * The copy is the whole of what can be a number, however long: it was
 * cut at 511 characters, so a longer decimal rounded by its first 511
 * digits only and its end was put at the cut.
 */
#include <wchar.h>
#include <stdlib.h>

/* The longest start of s a number can be: white space, then the
 * characters of a numeral, inf, nan and nan(...). Every one is below 128,
 * which is what makes the copy a byte copy. It goes on the stack up to
 * this many characters, and is allocated beyond -- a 900-digit decimal
 * still rounds by all its digits, and its end is still found. */
#define STACKNUM 128

static size_t numlen(const wchar_t *s)
{
    size_t i = 0;
    while (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))
        i++;
    for (; s[i]; i++) {
        wchar_t c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == '.' || c == '+' || c == '-' ||
              c == '(' || c == ')' || c == '_'))
            break;
    }
    return i;
}

/* s's number as bytes, in stack (STACKNUM bytes) or allocated; NULL if
 * an allocation failed */
static char *narrow(const wchar_t *s, char *stack)
{
    size_t n = numlen(s);
    char *buf = n < STACKNUM ? stack : malloc(n + 1);
    if (!buf)
        return NULL;
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)s[i];
    buf[n] = 0;
    return buf;
}

/* The end pointer, mapped from a byte offset back to a character
 * offset. They are the same number, because every byte that was copied
 * came from exactly one wchar_t below 128. */
#define WIDE(T, name, call)                                                 \
    T name                                                                  \
    {                                                                       \
        char stack[STACKNUM], *e = NULL, *buf = narrow(s, stack);           \
        if (!buf) {                                                         \
            if (end) *end = (wchar_t *)s;                                   \
            return 0;                                                       \
        }                                                                   \
        T v = call;                                                         \
        if (end) *end = (wchar_t *)s + (size_t)(e - buf);                   \
        if (buf != stack) free(buf);                                        \
        return v;                                                           \
    }

WIDE(long, wcstol(const wchar_t *__restrict s, wchar_t **__restrict end,
                  int base), strtol(buf, &e, base))
WIDE(unsigned long, wcstoul(const wchar_t *__restrict s,
                            wchar_t **__restrict end, int base),
     strtoul(buf, &e, base))
WIDE(long long, wcstoll(const wchar_t *__restrict s, wchar_t **__restrict end,
                        int base), strtoll(buf, &e, base))
WIDE(unsigned long long, wcstoull(const wchar_t *__restrict s,
                                  wchar_t **__restrict end, int base),
     strtoull(buf, &e, base))
WIDE(double, wcstod(const wchar_t *__restrict s, wchar_t **__restrict end),
     strtod(buf, &e))
WIDE(float, wcstof(const wchar_t *__restrict s, wchar_t **__restrict end),
     strtof(buf, &e))
WIDE(long double, wcstold(const wchar_t *__restrict s,
                          wchar_t **__restrict end), strtold(buf, &e))
