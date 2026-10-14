#include <stdio.h>
#include <stdint.h>
int main(void)
{
#define P(n, d) { uint64_t a = n, b = d; int64_t sa = (int64_t)a, sb = (int64_t)b; \
    printf("%llx %llx %llx %llx\n", (unsigned long long)(a / b), (unsigned long long)(a % b), \
        sb == -1 && sa == INT64_MIN ? 0ULL : (unsigned long long)(sa / sb), \
        sb == -1 && sa == INT64_MIN ? 0ULL : (unsigned long long)(sa % sb)); }
#include "pairs.h"
    printf("cycles fast\n==END==\n");
    return 0;
}
