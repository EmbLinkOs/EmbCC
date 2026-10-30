/* <malloc.h> -- glibc's: the allocator's own questions and knobs, beside
 * <stdlib.h>'s malloc family. mallinfo and malloc_stats walk the heap;
 * mallopt and malloc_trim are accepted and change nothing, as the heap
 * has no thresholds to set and gives no memory back. */
#ifndef _MALLOC_H
#define _MALLOC_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mallinfo {
    int arena;       /* bytes the heap has taken from the system */
    int ordblks;     /* free blocks */
    int smblks, hblks, hblkhd, usmblks, fsmblks;   /* always 0 here */
    int uordblks;    /* bytes in use */
    int fordblks;    /* bytes free */
    int keepcost;    /* the free bytes at the top of the heap */
};

struct mallinfo2 {
    size_t arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks;
    size_t uordblks, fordblks, keepcost;
};

/* mallopt's parameters, glibc's numbers */
#define M_MXFAST          1
#define M_NLBLKS          2
#define M_GRAIN           3
#define M_KEEP            4
#define M_TRIM_THRESHOLD  (-1)
#define M_TOP_PAD         (-2)
#define M_MMAP_THRESHOLD  (-3)
#define M_MMAP_MAX        (-4)
#define M_CHECK_ACTION    (-5)
#define M_PERTURB         (-6)
#define M_ARENA_TEST      (-7)
#define M_ARENA_MAX       (-8)

size_t malloc_usable_size(void *p);
void  *memalign(size_t align, size_t n);
void  *valloc(size_t n);
void  *pvalloc(size_t n);
struct mallinfo  mallinfo(void);
struct mallinfo2 mallinfo2(void);
void   malloc_stats(void);
int    malloc_trim(size_t pad);
int    mallopt(int param, int value);

#ifdef __cplusplus
}
#endif

#endif
