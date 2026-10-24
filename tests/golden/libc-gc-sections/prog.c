/* A firmware-sized use of lib/libc: printf, a float function, strtof,
 * qsort and strlen. Each is in a source file with neighbours this program
 * never calls -- cosh beside expf in math.c, strtold beside strtof,
 * fprintf beside printf, fopen in stdio.c -- which an image linked with
 * --gc-sections must leave out. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the bare-metal backend's console: the harness's output routine */
extern void writec(int c);
long write(int fd, const void *buf, unsigned long n)
{
    const unsigned char *p = buf;
    (void)fd;
    for (unsigned long i = 0; i < n; i++)
        writec(p[i]);
    return (long)n;
}

static int cmp(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int main(void)
{
    int v[5] = { 4, 1, 5, 2, 3 };
    float f = strtof("0.5", NULL);
    qsort(v, 5, sizeof v[0], cmp);
    printf("sorted %d%d%d%d%d\n", v[0], v[1], v[2], v[3], v[4]);
    printf("expf %.4f sqrtf %.4f\n", (double)expf(f), (double)sqrtf(f));
    printf("len %u\n", (unsigned)strlen("hello"));
    printf("==END==\n");
    return 0;
}
