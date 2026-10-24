/* <inttypes.h>'s functions.
 *
 * They are one-liners because intmax_t is long long or long -- the same
 * width -- on every target, so these ARE strtoll/strtoull/wcstoll/
 * wcstoull. Spelling them as calls rather than as aliases keeps that a
 * fact about these targets rather than a promise: on a target where
 * intmax_t is wider, only this file changes.
 */
#include <inttypes.h>
#include <stdlib.h>
#include <wchar.h>

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

intmax_t wcstoimax(const wchar_t *restrict s, wchar_t **restrict end, int base)
{
    return (intmax_t)wcstoll(s, end, base);
}

uintmax_t wcstoumax(const wchar_t *restrict s, wchar_t **restrict end,
                    int base)
{
    return (uintmax_t)wcstoull(s, end, base);
}
