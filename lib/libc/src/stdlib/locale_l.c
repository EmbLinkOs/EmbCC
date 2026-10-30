/* The _l functions: each with a locale argument, which here can only
 * be the one locale there is ("C", <locale.h>), so each is its plain
 * counterpart. They exist because POSIX has them and a program written
 * for several locales calls them. */
#define _GNU_SOURCE 1
#include <ctype.h>
#include <langinfo.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#define WEAK __attribute__((weak))

WEAK int isalnum_l(int c, locale_t l)
{
    (void)l;
    return isalnum(c);
}

WEAK int isalpha_l(int c, locale_t l)
{
    (void)l;
    return isalpha(c);
}

WEAK int isblank_l(int c, locale_t l)
{
    (void)l;
    return isblank(c);
}

WEAK int iscntrl_l(int c, locale_t l)
{
    (void)l;
    return iscntrl(c);
}

WEAK int isdigit_l(int c, locale_t l)
{
    (void)l;
    return isdigit(c);
}

WEAK int isgraph_l(int c, locale_t l)
{
    (void)l;
    return isgraph(c);
}

WEAK int islower_l(int c, locale_t l)
{
    (void)l;
    return islower(c);
}

WEAK int isprint_l(int c, locale_t l)
{
    (void)l;
    return isprint(c);
}

WEAK int ispunct_l(int c, locale_t l)
{
    (void)l;
    return ispunct(c);
}

WEAK int isspace_l(int c, locale_t l)
{
    (void)l;
    return isspace(c);
}

WEAK int isupper_l(int c, locale_t l)
{
    (void)l;
    return isupper(c);
}

WEAK int isxdigit_l(int c, locale_t l)
{
    (void)l;
    return isxdigit(c);
}

WEAK int tolower_l(int c, locale_t l)
{
    (void)l;
    return tolower(c);
}

WEAK int toupper_l(int c, locale_t l)
{
    (void)l;
    return toupper(c);
}

WEAK int iswalnum_l(wint_t c, locale_t l)
{
    (void)l;
    return iswalnum(c);
}

WEAK int iswalpha_l(wint_t c, locale_t l)
{
    (void)l;
    return iswalpha(c);
}

WEAK int iswblank_l(wint_t c, locale_t l)
{
    (void)l;
    return iswblank(c);
}

WEAK int iswcntrl_l(wint_t c, locale_t l)
{
    (void)l;
    return iswcntrl(c);
}

WEAK int iswdigit_l(wint_t c, locale_t l)
{
    (void)l;
    return iswdigit(c);
}

WEAK int iswgraph_l(wint_t c, locale_t l)
{
    (void)l;
    return iswgraph(c);
}

WEAK int iswlower_l(wint_t c, locale_t l)
{
    (void)l;
    return iswlower(c);
}

WEAK int iswprint_l(wint_t c, locale_t l)
{
    (void)l;
    return iswprint(c);
}

WEAK int iswpunct_l(wint_t c, locale_t l)
{
    (void)l;
    return iswpunct(c);
}

WEAK int iswspace_l(wint_t c, locale_t l)
{
    (void)l;
    return iswspace(c);
}

WEAK int iswupper_l(wint_t c, locale_t l)
{
    (void)l;
    return iswupper(c);
}

WEAK int iswxdigit_l(wint_t c, locale_t l)
{
    (void)l;
    return iswxdigit(c);
}

WEAK int iswctype_l(wint_t c, wctype_t t, locale_t l)
{
    (void)l;
    return iswctype(c, t);
}

WEAK wint_t towlower_l(wint_t c, locale_t l)
{
    (void)l;
    return towlower(c);
}

WEAK wint_t towupper_l(wint_t c, locale_t l)
{
    (void)l;
    return towupper(c);
}

WEAK wint_t towctrans_l(wint_t c, wctrans_t t, locale_t l)
{
    (void)l;
    return towctrans(c, t);
}

WEAK wctype_t wctype_l(const char *name, locale_t l)
{
    (void)l;
    return wctype(name);
}

WEAK wctrans_t wctrans_l(const char *name, locale_t l)
{
    (void)l;
    return wctrans(name);
}

WEAK int strcoll_l(const char *a, const char *b, locale_t l)
{
    (void)l;
    return strcoll(a, b);
}

WEAK size_t strxfrm_l(char *restrict d, const char *restrict s, size_t n, locale_t l)
{
    (void)l;
    return strxfrm(d, s, n);
}

WEAK char *strerror_l(int err, locale_t l)
{
    (void)l;
    return strerror(err);
}

WEAK int strcasecmp_l(const char *a, const char *b, locale_t l)
{
    (void)l;
    return strcasecmp(a, b);
}

WEAK int strncasecmp_l(const char *a, const char *b, size_t n, locale_t l)
{
    (void)l;
    return strncasecmp(a, b, n);
}

WEAK double strtod_l(const char *restrict s, char **restrict end, locale_t l)
{
    (void)l;
    return strtod(s, end);
}

WEAK float strtof_l(const char *restrict s, char **restrict end, locale_t l)
{
    (void)l;
    return strtof(s, end);
}

WEAK long double strtold_l(const char *restrict s, char **restrict end, locale_t l)
{
    (void)l;
    return strtold(s, end);
}

WEAK long strtol_l(const char *restrict s, char **restrict end, int base, locale_t l)
{
    (void)l;
    return strtol(s, end, base);
}

WEAK unsigned long strtoul_l(const char *restrict s, char **restrict end, int base, locale_t l)
{
    (void)l;
    return strtoul(s, end, base);
}

WEAK long long strtoll_l(const char *restrict s, char **restrict end, int base, locale_t l)
{
    (void)l;
    return strtoll(s, end, base);
}

WEAK unsigned long long strtoull_l(const char *restrict s, char **restrict end, int base, locale_t l)
{
    (void)l;
    return strtoull(s, end, base);
}

WEAK int wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t l)
{
    (void)l;
    return wcscoll(a, b);
}

WEAK size_t wcsxfrm_l(wchar_t *restrict d, const wchar_t *restrict s, size_t n, locale_t l)
{
    (void)l;
    return wcsxfrm(d, s, n);
}

WEAK int wcscasecmp_l(const wchar_t *a, const wchar_t *b, locale_t l)
{
    (void)l;
    return wcscasecmp(a, b);
}

WEAK int wcsncasecmp_l(const wchar_t *a, const wchar_t *b, size_t n, locale_t l)
{
    (void)l;
    return wcsncasecmp(a, b, n);
}

WEAK double wcstod_l(const wchar_t *restrict s, wchar_t **restrict end, locale_t l)
{
    (void)l;
    return wcstod(s, end);
}

WEAK float wcstof_l(const wchar_t *restrict s, wchar_t **restrict end, locale_t l)
{
    (void)l;
    return wcstof(s, end);
}

WEAK long double wcstold_l(const wchar_t *restrict s, wchar_t **restrict end, locale_t l)
{
    (void)l;
    return wcstold(s, end);
}

WEAK long wcstol_l(const wchar_t *restrict s, wchar_t **restrict end, int base, locale_t l)
{
    (void)l;
    return wcstol(s, end, base);
}

WEAK unsigned long wcstoul_l(const wchar_t *restrict s, wchar_t **restrict end, int base, locale_t l)
{
    (void)l;
    return wcstoul(s, end, base);
}

WEAK long long wcstoll_l(const wchar_t *restrict s, wchar_t **restrict end, int base, locale_t l)
{
    (void)l;
    return wcstoll(s, end, base);
}

WEAK unsigned long long wcstoull_l(const wchar_t *restrict s, wchar_t **restrict end, int base, locale_t l)
{
    (void)l;
    return wcstoull(s, end, base);
}

WEAK size_t strftime_l(char *restrict s, size_t n, const char *restrict fmt, const struct tm *restrict tm, locale_t l)
{
    (void)l;
    return strftime(s, n, fmt, tm);
}

WEAK size_t wcsftime_l(wchar_t *restrict s, size_t n, const wchar_t *restrict fmt, const struct tm *restrict tm, locale_t l)
{
    (void)l;
    return wcsftime(s, n, fmt, tm);
}

WEAK char *strptime_l(const char *restrict s, const char *restrict fmt, struct tm *restrict tm, locale_t l)
{
    (void)l;
    return strptime(s, fmt, tm);
}

WEAK char *nl_langinfo_l(nl_item item, locale_t l)
{
    (void)l;
    return nl_langinfo(item);
}
