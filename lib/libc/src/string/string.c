/* <string.h>, C11 §7.24.
 *
 * Correct first. Where a word-at-a-time loop is free it is used, because
 * these are the functions every program spends time in and the compiler
 * emits calls to them itself (memcpy/memset for aggregate copies and
 * initializers, §11) — but never at the cost of reading past the end of an
 * object, which a word-at-a-time strlen must be careful about and which is
 * why this one walks bytes until it is word-aligned first.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>

typedef unsigned long word;
#define WSZ ((size_t)sizeof(word))

void *memcpy(void *restrict d, const void *restrict s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    /* Word copies once both sides share an alignment; the tails are bytes.
     * The objects do not overlap (that is memmove's job), so direction
     * does not matter. */
    if (n >= WSZ && ((size_t)p % WSZ) == ((size_t)q % WSZ)) {
        while ((size_t)p % WSZ) { *p++ = *q++; n--; }
        while (n >= WSZ) { *(word *)p = *(const word *)q; p += WSZ; q += WSZ; n -= WSZ; }
    }
    while (n--) *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p == q || n == 0)
        return d;
    /* Overlapping and the destination is above the source: copy downward,
     * or the bytes still to be read are overwritten first. */
    if (p < q)
        return memcpy(d, s, n);
    p += n; q += n;
    while (n--) *--p = *--q;
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    unsigned char v = (unsigned char)c;
    if (n >= WSZ) {
        while ((size_t)p % WSZ) { *p++ = v; n--; }
        word w = 0;
        for (size_t i = 0; i < WSZ; i++) w = (w << 8) | v;
        while (n >= WSZ) { *(word *)p = w; p += WSZ; n -= WSZ; }
    }
    while (n--) *p++ = v;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n--; x++, y++)
        if (*x != *y)
            return *x < *y ? -1 : 1;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    unsigned char v = (unsigned char)c;
    for (; n--; p++)
        if (*p == v)
            return (void *)p;
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i]) i++;
    return i;
}

char *strcpy(char *restrict d, const char *restrict s)
{
    char *r = d;
    while ((*d++ = *s++)) {}
    return r;
}

char *strncpy(char *restrict d, const char *restrict s, size_t n)
{
    char *r = d;
    /* C says: copy at most n, and PAD with NULs to n. The padding is the
     * part people forget; it is what makes strncpy not a safe strcpy. */
    while (n && (*d = *s)) { d++; s++; n--; }
    while (n--) *d++ = 0;
    return r;
}

char *strcat(char *restrict d, const char *restrict s)
{
    char *r = d;
    while (*d) d++;
    while ((*d++ = *s++)) {}
    return r;
}

char *strncat(char *restrict d, const char *restrict s, size_t n)
{
    char *r = d;
    while (*d) d++;
    while (n && *s) { *d++ = *s++; n--; }
    *d = 0;                       /* always terminated, unlike strncpy */
    return r;
}

int strcmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *x == *y) { x++; y++; }
    return *x < *y ? -1 : *x > *y;
}

int strncmp(const char *a, const char *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n && *x && *x == *y) { x++; y++; n--; }
    if (!n) return 0;
    return *x < *y ? -1 : *x > *y;
}

/* The "C" locale is the only one, so collation is comparison and
 * transformation is a copy. Provided because C requires them and a program
 * that calls them should link. */
int strcoll(const char *a, const char *b) { return strcmp(a, b); }

size_t strxfrm(char *restrict d, const char *restrict s, size_t n)
{
    size_t len = strlen(s);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return len;
}

char *strchr(const char *s, int c)
{
    char v = (char)c;
    for (;; s++) {
        if (*s == v) return (char *)s;
        if (!*s) return NULL;     /* c == 0 finds the terminator: C says so */
    }
}

char *strrchr(const char *s, int c)
{
    char v = (char)c;
    const char *last = NULL;
    for (;; s++) {
        if (*s == v) last = s;
        if (!*s) return (char *)last;
    }
}

size_t strspn(const char *s, const char *acc)
{
    const char *p = s;
    for (; *p && strchr(acc, *p); p++) {}
    return (size_t)(p - s);
}

size_t strcspn(const char *s, const char *rej)
{
    const char *p = s;
    for (; *p && !strchr(rej, *p); p++) {}
    return (size_t)(p - s);
}

char *strpbrk(const char *s, const char *acc)
{
    for (; *s; s++)
        if (strchr(acc, *s))
            return (char *)s;
    return NULL;
}

char *strstr(const char *h, const char *n)
{
    if (!*n)
        return (char *)h;          /* an empty needle matches at the front */
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return NULL;
}

char *strtok(char *restrict s, const char *restrict sep)
{
    static char *save;
    if (!s) s = save;
    if (!s) return NULL;
    s += strspn(s, sep);           /* skip leading separators */
    if (!*s) { save = NULL; return NULL; }
    char *tok = s;
    s += strcspn(s, sep);
    if (*s) { *s = 0; save = s + 1; }
    else      save = NULL;
    return tok;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = 0; }
    return p;
}
