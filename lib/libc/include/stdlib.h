/* <stdlib.h> — C11 §7.22. */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>
#include <features.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
/* The locale's longest multibyte character. There is one locale, and
 * its encoding is UTF-8. */
#define MB_CUR_MAX ((size_t)4)

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);
void *aligned_alloc(size_t align, size_t n);
/* C23: free, told the size (and alignment) the block was asked for -- this
 * allocator keeps both in the block, so they free as free does -- and the
 * alignment an address has, its lowest set bit. */
void   free_sized(void *p, size_t size);
void   free_aligned_sized(void *p, size_t align, size_t size);
size_t memalignment(const void *p);

double             strtod(const char *__restrict s, char **__restrict end);
float              strtof(const char *__restrict s, char **__restrict end);
long double        strtold(const char *__restrict s, char **__restrict end);
long               strtol(const char *__restrict s, char **__restrict end, int base);
long long          strtoll(const char *__restrict s, char **__restrict end, int base);
unsigned long      strtoul(const char *__restrict s, char **__restrict end, int base);
unsigned long long strtoull(const char *__restrict s, char **__restrict end, int base);

/* C23: one floating value as a printf conversion writes it, the format
 * being "%" with an optional precision and one of aAeEfFgG. */
int strfromd(char *__restrict s, size_t n, const char *__restrict fmt,
             double fp);
int strfromf(char *__restrict s, size_t n, const char *__restrict fmt,
             float fp);
int strfroml(char *__restrict s, size_t n, const char *__restrict fmt,
             long double fp);

int       atoi(const char *s);
long      atol(const char *s);
long long atoll(const char *s);
double    atof(const char *s);

int  rand(void);
void srand(unsigned seed);

void  abort(void);
int   atexit(void (*f)(void));
/* Not C11, but the form C++ static destructors need -- and it shares
 * atexit's list, so the two interleave by registration order. The C++
 * runtime calls it; see lib/libc/src/stdlib/exit.c. */
int   __cxa_atexit(void (*f)(void *), void *arg, void *dso);
void  __cxa_finalize(void *dso);
void  exit(int status);
void  _Exit(int status);
/* The exit that runs only what at_quick_exit registered (C11 7.22.4.7):
 * no atexit handlers, no destructors, no flush. */
int   at_quick_exit(void (*f)(void));
void  quick_exit(int status);
/* C23 declares call_once here as well as in <threads.h>. */
#if __STDC_VERSION__ >= 202311L
#ifndef __EMBLIBC_ONCE_FLAG
#define __EMBLIBC_ONCE_FLAG
typedef struct { int __state; } once_flag;
#define ONCE_FLAG_INIT { 0 }
#endif
void call_once(once_flag *flag, void (*func)(void));
#endif
char *getenv(const char *name);
/* The vector getenv reads; the program's startup code publishes it. */
extern char **environ;
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   system(const char *cmd);

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));
void  qsort(void *base, size_t n, size_t size,
            int (*cmp)(const void *, const void *));

/* Multibyte characters without a state (7.22.7, 7.22.8): UTF-8, as the
 * restartable forms in <wchar.h> are. */
int    mblen(const char *s, size_t n);
int    mbtowc(wchar_t *__restrict pwc, const char *__restrict s, size_t n);
int    wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *__restrict d, const char *__restrict s, size_t n);
size_t wcstombs(char *__restrict d, const wchar_t *__restrict s, size_t n);

int       abs(int x);
long      labs(long x);
long long llabs(long long x);
div_t     div(int num, int den);
ldiv_t    ldiv(long num, long den);
lldiv_t   lldiv(long long num, long long den);

/* POSIX's, which ISO C leaves to the program (see <features.h>) */
#if defined(__EMB_POSIX)
int   putenv(char *s);
int   mkstemp(char *tmpl);
int   mkostemp(char *tmpl, int flags);
char *mkdtemp(char *tmpl);
char *mktemp(char *tmpl);
char *realpath(const char *__restrict path, char *__restrict resolved);
int   posix_memalign(void **p, size_t align, size_t n);
void *reallocarray(void *p, size_t n, size_t size);
void  qsort_r(void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *, void *), void *arg);
int   getsubopt(char **opt, char *const *tokens, char **value);

/* the historical generators, each giving glibc's sequence for a seed */
long  random(void);
void  srandom(unsigned seed);
char *initstate(unsigned seed, char *state, size_t n);
char *setstate(char *state);
int   rand_r(unsigned *seed);
double drand48(void);
double erand48(unsigned short x[3]);
long   lrand48(void);
long   nrand48(unsigned short x[3]);
long   mrand48(void);
long   jrand48(unsigned short x[3]);
void   srand48(long seed);
unsigned short *seed48(unsigned short s[3]);
void   lcong48(unsigned short p[7]);
#endif

#if defined(__EMB_GNU)
int   clearenv(void);
int   mkstemps(char *tmpl, int suffix);
int   mkostemps(char *tmpl, int suffix, int flags);
void *memalign(size_t align, size_t n);
const char *getprogname(void);
void  setprogname(const char *name);
#endif

#ifdef __cplusplus
}
#endif

#endif
