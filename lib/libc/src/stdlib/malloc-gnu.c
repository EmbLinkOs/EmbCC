/* <malloc.h>'s functions that are not the allocator's own business: page-
 * aligned blocks, and the report. Apart from malloc.c, so that a program
 * that only allocates does not bring printf along with it. */
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>

static size_t page(void)
{
    long n = sysconf(_SC_PAGESIZE);
    return n > 0 ? (size_t)n : 4096;
}

__attribute__((weak)) void *valloc(size_t n)
{
    return memalign(page(), n);
}

/* valloc of a size rounded up to whole pages; 0 is a page, as glibc's */
__attribute__((weak)) void *pvalloc(size_t n)
{
    size_t p = page();
    if (n > (size_t)-1 - p)
        return NULL;
    return memalign(p, n ? (n + p - 1) & ~(p - 1) : p);
}

/* glibc's report, for the one arena there is */
__attribute__((weak)) void malloc_stats(void)
{
    struct mallinfo2 m = mallinfo2();
    fprintf(stderr, "Arena 0:\nsystem bytes     = %10zu\n"
                    "in use bytes     = %10zu\n"
                    "Total (incl. mmap):\nsystem bytes     = %10zu\n"
                    "in use bytes     = %10zu\nmax mmap regions = %10u\n"
                    "max mmap bytes   = %10lu\n",
            m.arena, m.uordblks, m.arena, m.uordblks, 0u, 0ul);
}
