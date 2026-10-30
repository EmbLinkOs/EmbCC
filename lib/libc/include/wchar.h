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

#include <features.h>

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
#if defined(__APPLE__)
/* Apple's C library's own, which a Darwin program's mbrtowc and the rest
 * write into: 128 bytes, where this library's is 12 -- so the 12-byte one
 * was overrun by every conversion that kept state. */
typedef union {
    char __mbstate8[128];
    long long _mbstateL;
} mbstate_t;
#else
typedef struct {
    __UINT_LEAST32_TYPE__ __wch;      /* the code point being assembled */
    unsigned char __want;             /* bytes still needed (0: none pending) */
    unsigned char __have;             /* bytes taken so far */
    __UINT_LEAST32_TYPE__ __lowbound; /* the smallest value this length may encode */
} mbstate_t;
#endif

struct tm;

#ifndef __EMBLIBC_FILE
#define __EMBLIBC_FILE
typedef struct _FILE FILE;
#endif

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
wint_t btowc(int c);
int wctob(wint_t c);
size_t mbrlen(const char *__restrict s, size_t n, mbstate_t *__restrict ps);
size_t mbrtowc(wchar_t *__restrict pwc, const char *__restrict s, size_t n,
               mbstate_t *__restrict ps);
size_t wcrtomb(char *__restrict s, wchar_t wc, mbstate_t *__restrict ps);
size_t mbsrtowcs(wchar_t *__restrict d, const char **__restrict s, size_t n,
                 mbstate_t *__restrict ps);
size_t wcsrtombs(char *__restrict d, const wchar_t **__restrict s, size_t n,
                 mbstate_t *__restrict ps);

/* ---- wide-character input and output (7.29.3) ----
 *
 * A wide stream's bytes are UTF-8. The first byte or wide operation on a
 * stream decides which it is, and fwide asks (or decides). */
int fwide(FILE *f, int mode);
wint_t fgetwc(FILE *f);
wint_t getwc(FILE *f);
wint_t getwchar(void);
wint_t fputwc(wchar_t c, FILE *f);
wint_t putwc(wchar_t c, FILE *f);
wint_t putwchar(wchar_t c);
wint_t ungetwc(wint_t c, FILE *f);
wchar_t *fgetws(wchar_t *__restrict s, int n, FILE *__restrict f);
int fputws(const wchar_t *__restrict s, FILE *__restrict f);

/* ---- formatted wide input and output (7.29.2) ---- */
int fwprintf(FILE *__restrict f, const wchar_t *__restrict fmt, ...);
int wprintf(const wchar_t *__restrict fmt, ...);
int swprintf(wchar_t *__restrict s, size_t n,
             const wchar_t *__restrict fmt, ...);
int vfwprintf(FILE *__restrict f, const wchar_t *__restrict fmt,
              va_list ap);
int vwprintf(const wchar_t *__restrict fmt, va_list ap);
int vswprintf(wchar_t *__restrict s, size_t n,
              const wchar_t *__restrict fmt, va_list ap);
int fwscanf(FILE *__restrict f, const wchar_t *__restrict fmt, ...);
int wscanf(const wchar_t *__restrict fmt, ...);
int swscanf(const wchar_t *__restrict s, const wchar_t *__restrict fmt, ...);
int vfwscanf(FILE *__restrict f, const wchar_t *__restrict fmt, va_list ap);
int vwscanf(const wchar_t *__restrict fmt, va_list ap);
int vswscanf(const wchar_t *__restrict s, const wchar_t *__restrict fmt,
             va_list ap);

/* ---- time (7.29.5) ---- */
size_t wcsftime(wchar_t *__restrict s, size_t max,
                const wchar_t *__restrict fmt,
                const struct tm *__restrict tm);

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

#if defined(__EMB_POSIX)
#include <bits/locale_t.h>
int     wcscoll_l(const wchar_t *a, const wchar_t *b, locale_t l);
size_t  wcsxfrm_l(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n, locale_t l);
int     wcscasecmp(const wchar_t *a, const wchar_t *b);
int     wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n);
int     wcscasecmp_l(const wchar_t *a, const wchar_t *b, locale_t l);
int     wcsncasecmp_l(const wchar_t *a, const wchar_t *b, size_t n, locale_t l);
wchar_t *wcpcpy(wchar_t *__restrict d, const wchar_t *__restrict s);
wchar_t *wcpncpy(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n);
wchar_t *wcsdup(const wchar_t *s);
size_t  wcsnrtombs(char *__restrict d, const wchar_t **__restrict s, size_t nwc,
                   size_t len, mbstate_t *__restrict ps);
size_t  mbsnrtowcs(wchar_t *__restrict d, const char **__restrict s, size_t nms,
                   size_t len, mbstate_t *__restrict ps);
int     wcwidth(wchar_t c);
int     wcswidth(const wchar_t *s, size_t n);
#endif
#if defined(__EMB_GNU)
wint_t  fgetwc_unlocked(FILE *f);
wint_t  fputwc_unlocked(wchar_t c, FILE *f);
wint_t  getwc_unlocked(FILE *f);
wint_t  putwc_unlocked(wchar_t c, FILE *f);
wint_t  getwchar_unlocked(void);
wint_t  putwchar_unlocked(wchar_t c);
wchar_t *fgetws_unlocked(wchar_t *__restrict s, int n, FILE *__restrict f);
int     fputws_unlocked(const wchar_t *__restrict s, FILE *__restrict f);
wchar_t *wmempcpy(wchar_t *__restrict d, const wchar_t *__restrict s, size_t n);
wchar_t *wcschrnul(const wchar_t *s, wchar_t c);
wchar_t *wcswcs(const wchar_t *big, const wchar_t *little);
double  wcstod_l(const wchar_t *__restrict s, wchar_t **__restrict end, locale_t l);
float   wcstof_l(const wchar_t *__restrict s, wchar_t **__restrict end, locale_t l);
long double wcstold_l(const wchar_t *__restrict s, wchar_t **__restrict end, locale_t l);
long    wcstol_l(const wchar_t *__restrict s, wchar_t **__restrict end, int base, locale_t l);
unsigned long wcstoul_l(const wchar_t *__restrict s, wchar_t **__restrict end, int base, locale_t l);
long long wcstoll_l(const wchar_t *__restrict s, wchar_t **__restrict end, int base, locale_t l);
unsigned long long wcstoull_l(const wchar_t *__restrict s, wchar_t **__restrict end, int base, locale_t l);
size_t  wcsftime_l(wchar_t *__restrict s, size_t n, const wchar_t *__restrict fmt,
                   const struct tm *__restrict tm, locale_t l);
#endif

#ifdef __cplusplus
}
#endif

#endif
