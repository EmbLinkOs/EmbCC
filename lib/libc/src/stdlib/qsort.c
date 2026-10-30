/* qsort and bsearch, C11 §7.22.5.
 *
 * Heapsort, not quicksort. C does not require a stable sort or a particular
 * complexity, but it also does not excuse quadratic behaviour on an input
 * an attacker chose, and a library sort is exactly where such an input
 * arrives. Heapsort is n log n on every input, needs no recursion and no
 * scratch memory, and is short enough to be obviously right.
 */
#include <stdlib.h>
#include <string.h>

static void swap(char *a, char *b, size_t size)
{
    while (size--) { char t = *a; *a++ = *b; *b++ = t; }
}

static void sift(char *base, size_t size, size_t root, size_t n,
                 int (*cmp)(const void *, const void *, void *), void *arg)
{
    for (;;) {
        size_t big = root, l = 2 * root + 1, r = l + 1;
        if (l < n && cmp(base + l * size, base + big * size, arg) > 0) big = l;
        if (r < n && cmp(base + r * size, base + big * size, arg) > 0) big = r;
        if (big == root)
            return;
        swap(base + root * size, base + big * size, size);
        root = big;
    }
}

/* POSIX.1-2024's qsort_r, the comparison told the caller's `arg` -- in
 * glibc's argument order, which POSIX adopted, and not the BSDs' older
 * one with arg first. */
__attribute__((weak)) void qsort_r(void *base, size_t n, size_t size,
             int (*cmp)(const void *, const void *, void *), void *arg)
{
    if (n < 2 || size == 0)
        return;
    char *b = base;
    for (size_t i = n / 2; i-- > 0; )
        sift(b, size, i, n, cmp, arg);
    for (size_t i = n; i-- > 1; ) {
        swap(b, b + i * size, size);
        sift(b, size, 0, i, cmp, arg);
    }
}

/* C's two-argument comparison, carried as qsort_r's arg. In a struct,
 * because ISO C has no conversion between a function pointer and void *. */
struct cmp2 { int (*f)(const void *, const void *); };

static int call2(const void *a, const void *b, void *c)
{
    return ((struct cmp2 *)c)->f(a, b);
}

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *))
{
    struct cmp2 c = { cmp };
    qsort_r(base, n, size, call2, &c);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *b = base;
    while (n) {
        size_t mid = n / 2;
        const char *p = b + mid * size;
        int r = cmp(key, p);
        if (r == 0) return (void *)p;
        if (r > 0) { b = p + size; n -= mid + 1; }
        else       { n = mid; }
    }
    return NULL;
}
