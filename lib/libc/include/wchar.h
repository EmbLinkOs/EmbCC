/* <wchar.h> — C11 §7.29.
 *
 * Wide characters, which on this target means UTF-32: `wchar_t` is a
 * 32-bit int, so one wchar_t is one Unicode code point and there are no
 * surrogates to think about. That is the comfortable case, and it is
 * worth saying out loud because the uncomfortable one -- Windows, where
 * wchar_t is 16 bits and a code point may take two of them -- is why so
 * much wide-character code is subtly wrong.
 *
 * The multibyte conversions here are UTF-8 in both directions. C says
 * the multibyte encoding is locale-dependent; this library has one
 * locale (see <locale.h>) and its encoding is UTF-8, which is the only
 * choice that is not an arbitrary one.
 *
 * The conversion state (`mbstate_t`) is a real object here rather than
 * a placeholder: a partial UTF-8 sequence split across two calls has to
 * be remembered somewhere, and a function that ignores the state
 * silently mangles any input that does not arrive in whole characters.
 */
#ifndef _WCHAR_H
#define _WCHAR_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WCHAR_MIN
#define WCHAR_MIN __WCHAR_MIN__
#define WCHAR_MAX __WCHAR_MAX__
#endif

typedef __WINT_TYPE__ wint_t;

#define WEOF ((wint_t)-1)

/* A partially decoded multibyte character: the bytes seen so far and
 * how many are still wanted. A zeroed one means "between characters",
 * which is why memset is the documented way to reset it. */
typedef struct {
    unsigned int __wch;       /* the code point being assembled */
    unsigned char __want;     /* bytes still needed (0: none pending) */
    unsigned char __have;     /* bytes taken so far */
    unsigned int __lowbound;  /* the smallest value this length may encode */
} mbstate_t;

struct tm;

/* ---- wide string handling (7.29.4) ---- */
size_t wcslen(const wchar_t *s);
size_t wcsnlen(const wchar_t *s, size_t n);
wchar_t *wcscpy(wchar_t *__restrict d, const wchar_t *__restrict s);
wchar_t *wcsncpy(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n);
wchar_t *wcscat(wchar_t *__restrict d, const wchar_t *__restrict s);
wchar_t *wcsncat(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscoll(const wchar_t *a, const wchar_t *b);
size_t wcsxfrm(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *h, const wchar_t *n);
size_t wcsspn(const wchar_t *s, const wchar_t *set);
size_t wcscspn(const wchar_t *s, const wchar_t *set);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *set);
wchar_t *wcstok(wchar_t *__restrict s, const wchar_t *__restrict sep,
                wchar_t **__restrict save);

/* ---- wide memory (7.29.4.5) ---- */
wchar_t *wmemcpy(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n);
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

/* ---- conversion (7.29.6) ---- */
int mbsinit(const mbstate_t *ps);
size_t mbrlen(const char *__restrict s, size_t n, mbstate_t *__restrict ps);
size_t mbrtowc(wchar_t *__restrict pwc, const char *__restrict s, size_t n,
               mbstate_t *__restrict ps);
size_t wcrtomb(char *__restrict s, wchar_t wc, mbstate_t *__restrict ps);
size_t mbsrtowcs(wchar_t *__restrict d, const char **__restrict s, size_t n,
                 mbstate_t *__restrict ps);
size_t wcsrtombs(char *__restrict d, const wchar_t **__restrict s, size_t n,
                 mbstate_t *__restrict ps);

/* ---- numeric conversion (7.29.4.1) ---- */
long wcstol(const wchar_t *__restrict s, wchar_t **__restrict end, int base);
unsigned long wcstoul(const wchar_t *__restrict s, wchar_t **__restrict end,
                      int base);
long long wcstoll(const wchar_t *__restrict s, wchar_t **__restrict end,
                  int base);
unsigned long long wcstoull(const wchar_t *__restrict s,
                            wchar_t **__restrict end, int base);
double wcstod(const wchar_t *__restrict s, wchar_t **__restrict end);
float wcstof(const wchar_t *__restrict s, wchar_t **__restrict end);
long double wcstold(const wchar_t *__restrict s, wchar_t **__restrict end);

#ifdef __cplusplus
}
#endif

#endif
