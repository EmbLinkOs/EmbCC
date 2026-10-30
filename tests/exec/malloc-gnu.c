/* glibc's <malloc.h>: what an allocator may say about itself, checked
 * as properties any allocator has -- a block holds at least what was
 * asked, mallinfo2 counts a block while it is in use, valloc and pvalloc
 * are page-aligned and pvalloc rounds up to whole pages. (QuickJS sizes
 * its heap with malloc_usable_size; EmbLinkOS builds it on this.) */
// expect-exit: 42
// needs-libc: glibc's <malloc.h>, which newlib has only in part
#define _GNU_SOURCE 1
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    if (malloc_usable_size(NULL) != 0)
        return 1;
    for (size_t n = 1; n < 5000; n = n * 3 + 1) {
        unsigned char *p = malloc(n);
        size_t u = malloc_usable_size(p);
        if (!p || u < n)
            return 2;
        memset(p, 0xA5, u);                /* all of it is the program's */
        free(p);
    }
    char *a = aligned_alloc(64, 100);
    if (!a || malloc_usable_size(a) < 100)
        return 3;
    free(a);

    /* a block the counts must see: smaller where memory is a few KB */
    size_t bigsz = SIZE_MAX > 0xFFFF ? 100000 : 256;
    struct mallinfo2 before = mallinfo2();
    void *big = malloc(bigsz);
    struct mallinfo2 during = mallinfo2();
    if (!big || during.uordblks < before.uordblks + bigsz ||
        during.arena < during.uordblks)
        return 4;
    free(big);
    struct mallinfo2 after = mallinfo2();
    if (after.uordblks >= during.uordblks)
        return 5;

#if SIZE_MAX > 0xFFFF                 /* a page is more than an AVR has */
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        page = 4096;
    void *v = valloc(10);
    void *pv = pvalloc(1);
    if (!v || (uintptr_t)v % (uintptr_t)page || !pv ||
        (uintptr_t)pv % (uintptr_t)page ||
        malloc_usable_size(pv) < (size_t)page)
        return 6;
    free(v);
    free(pv);
#endif
    void *m = memalign(256, 33);
    if (!m || (uintptr_t)m % 256)
        return 7;
    free(m);
    return 42;
}
