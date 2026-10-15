/* The distance between two addresses in one object as a static
 * initializer: in a pointer-wide slot it was stored as an address. */
// expect-exit: 42
int printf(const char *, ...);
#include <stddef.h>
struct sv { int c; char pad[6]; int d; } v;
static short i = ((char *)&(v.c) - (char *)&v);
static long j = (char *)&v.d - (char *)&v.c;
int arr[60];
static long k = &arr[20] - &arr[5];
static long m = (long)&arr[3] - (long)&arr[1];
int main(void)
{
    long rt = &arr[9] - &arr[2];
    printf("%d %ld %ld %ld %ld\n", i, j, k, m, rt);
    /* d's offset is the target's (10 where int aligns to 2: ColdFire) */
    return i == 0 && j == (long)(offsetof(struct sv, d) - offsetof(struct sv, c)) &&
           k == 15 && m == 2 * (long)sizeof(int) && rt == 7 ? 42 : 1;
}
