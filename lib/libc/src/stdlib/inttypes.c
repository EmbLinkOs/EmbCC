/* <inttypes.h>'s four functions.
 *
 * They are one-liners here because intmax_t is `long` on both targets, so
 * these ARE strtol/strtoul/labs/ldiv. Spelling them as calls rather than
 * as aliases keeps that a fact about this target rather than a promise:
 * on a target where intmax_t is wider, only this file changes.
 */
#include <inttypes.h>
#include <stdlib.h>

intmax_t imaxabs(intmax_t j) { return j < 0 ? -j : j; }

imaxdiv_t imaxdiv(intmax_t num, intmax_t den)
{
    imaxdiv_t r;
    r.quot = num / den;
    r.rem = num % den;
    return r;
}

intmax_t strtoimax(const char *restrict s, char **restrict end, int base)
{
    return (intmax_t)strtoll(s, end, base);
}

uintmax_t strtoumax(const char *restrict s, char **restrict end, int base)
{
    return (uintmax_t)strtoull(s, end, base);
}
