/* The host's printf for the formats in fmt.h, as tests/golden/avr-libc-run.sh
 * compares them. */
#include <stdio.h>
int main(void)
{
#define F(...) printf(__VA_ARGS__), printf("\n");
#include "fmt.h"
    return 0;
}
