/* <stdlib.h> — C11 §7.22. */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void  free(void *p);
void *aligned_alloc(size_t align, size_t n);

double             strtod(const char *restrict s, char **restrict end);
float              strtof(const char *restrict s, char **restrict end);
long double        strtold(const char *restrict s, char **restrict end);
long               strtol(const char *restrict s, char **restrict end, int base);
long long          strtoll(const char *restrict s, char **restrict end, int base);
unsigned long      strtoul(const char *restrict s, char **restrict end, int base);
unsigned long long strtoull(const char *restrict s, char **restrict end, int base);

int       atoi(const char *s);
long      atol(const char *s);
long long atoll(const char *s);
double    atof(const char *s);

int  rand(void);
void srand(unsigned seed);

void  abort(void);
int   atexit(void (*f)(void));
void  exit(int status);
void  _Exit(int status);
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

int       abs(int x);
long      labs(long x);
long long llabs(long long x);
div_t     div(int num, int den);
ldiv_t    ldiv(long num, long den);
lldiv_t   lldiv(long long num, long long den);

#endif
