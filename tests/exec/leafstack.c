/* A leaf function whose last arguments arrive on the caller's stack.
 *
 * A leaf with nothing of its own on the stack builds no frame record on
 * aarch64 -- and a stack argument there is found through x29, which only
 * a frame record sets. So such a function must keep the record; built
 * without it, `ten` read its ninth and tenth arguments from wherever the
 * caller's x29 pointed. These are the leaves every target has: more
 * arguments than registers, and nothing else.
 */
// expect-exit: 42

__attribute__((noinline)) long ten(long a, long b, long c, long d, long e,
                                   long f, long g, long h, long i, long j)
{
    return a - b + c - d + e - f + g - h + i * 3 + j * 5;
}

__attribute__((noinline)) int nine(int a, int b, int c, int d, int e,
                                   int f, int g, int h, int i)
{
    return a + b + c + d + e + f + g + h + i * 10;
}

int main(void)
{
    if (ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 73)
        return 1;
    if (nine(1, 2, 3, 4, 5, 6, 7, 8, 9) != 126)
        return 2;
    if (ten(-5, 5, 0, 0, 0, 0, 0, 0, 1000, -600) != 3000 - 3000 - 10)
        return 3;
    return 42;
}
