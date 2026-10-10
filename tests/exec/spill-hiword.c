// expect-exit: 42
/* The high word of a 64-bit value that lives in a stack slot, read on its
 * own ((int)(x >> 32): ra_narrow_hishift) after a call: the slot must
 * hold it. Four such values outlive the call, more than there are
 * register pairs to keep them, so some live in the frame; on ARMv6-M the
 * high-word read is the only read of each, which the spill-store pass
 * (v6m.c, g6_velide) must count as one. */
#define NI __attribute__((noinline))

NI static long long mk(int a)
{
    return (long long)((unsigned long long)(unsigned)(a * 3 + 1) << 32 |
                       (unsigned)(a ^ 0x5a5a));
}
NI static int g(int b) { return b * 7; }

NI static int hw(int a, int b)
{
    long long x = mk(a), y = mk(b), z = mk(a + b), w = mk(a - b);
    int s = g(b);
    return (int)(x >> 32) + (int)(y >> 32) * 3 + (int)(z >> 32) * 5 +
           (int)(w >> 32) * 7 + s;
}

int main(void)
{
    int bad = 0;
    for (int a = -3; a < 4; a++)
        for (int b = 0; b < 3; b++) {
            int want = (a * 3 + 1) + (b * 3 + 1) * 3 + ((a + b) * 3 + 1) * 5 +
                       ((a - b) * 3 + 1) * 7 + b * 7;
            if (hw(a, b) != want)
                bad++;
        }
    return bad ? 1 : 42;
}
