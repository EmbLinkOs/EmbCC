/* What tests/golden/coldfire-asm/main.c should print, computed on the HOST
 * from the C meaning of each piece of assembly -- an independent model:
 * the board's numbers come from the assembler and the code generator, these
 * from the host's compiler. */
#include <stdio.h>

static int c_helper(int x) { return x * 7 + 1; }
static int c_helper2(int x) { return x * 3 - 2; }

static int live(int a, int b)
{
    return a * 3 + (b + 11) * 2 + (a ^ b) * 3 + (a - b) * 5;
}

int main(void)
{
    int a = 9, s;
    unsigned u;
    /* line 1: the inline asm */
    printf("%d %d %d %d %d %ld %d %d %d %d %d %d %d %d ", 42, 1, 7, 4, 0x1b,
           0x47ff0000L, 2, 21, 104, 31, 18, 9, 993, live(17, 5));
    printf("%d ", live(a, 3) + (a + 3 * 2) * 7 + a * 5 + a * 7 * 2 +
                  a * 11 * 3 + a * 13 * 4 + (a + 1) * 5 + (a ^ 99) * 6);
    printf("%d %d %d %d %d %d %d\n", 0x81 * 10 + 1, -100 / 7, -100 % 7,
           0x1234 << 4, 0x1234 >> 4, 10 + 20 + 5, 6 * -7);
    /* line 2: forms.S */
    printf("%d %d %d %d ", 3 + 1 + 4 + 1 + 5, c_helper(5) + c_helper2(5),
           c_helper(3), 4 + 1 + 2 + 3 + 4);
    printf("%d ", (100 + 7) * 3 - 7 - (100 & 7) + (100 | 7) + (100 ^ 7) +
                  (100 >> 2) + (100 << 3) + (int)((0u - 100u) >> 28));
    printf("%d %d %d %d %d %d ", 1, 0x8a, 10, 30, 8, 8);
    printf("%d ", -100 / 7 * 1000 + (-100 % 7) * 10 + (int)(100u / 7u));
    u = 0x12345680u;
    s = (int)(u << 16 | u >> 16);
    {
        unsigned d1 = ((unsigned)s & 0xffff0000u) |
                      ((unsigned)(short)(signed char)(s & 0xff) & 0xffffu);
        printf("%d ", (int)((unsigned)s + d1));
    }
    printf("%d %d\n", 37 + 5, 7);
    return 0;
}
