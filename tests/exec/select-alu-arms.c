// expect-exit: 42
/* Conditionals whose arm is arithmetic, which if-conversion runs on both
 * paths and selects between (an IT block on a Cortex-M, cmov or csel on
 * x86-64 and AArch64): one-armed increments in a character loop, add and
 * subtract arms, a shift guarded by its amount (run with amounts past the
 * width, where the discarded result must not matter), two-step arms, an
 * arm reading its own destination, 64-bit arms, and a division, which
 * must stay behind its test. Answers are counted
 * by hand. */
#include <stdint.h>

#define NI __attribute__((noinline))

NI static unsigned upper(const char *p)
{
    unsigned u = 0;
    for (; *p; p++)
        if (*p >= 'A' && *p <= 'Z') u++;
    return u;
}

NI static void counts(const char *p, unsigned out[3])
{
    unsigned digits = 0, spaces = 0, other = 0;
    for (; *p; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') digits += 2;
        if (c == ' ') spaces++;
        if (c == ',' || c == ';') other ^= (unsigned)c;
    }
    out[0] = digits; out[1] = spaces; out[2] = other;
}

NI static int addsub(int c, int a, int b) { return c ? a + b : a - b; }

NI static uint32_t guarded_shl(uint32_t x, unsigned n)
{
    if (n < 32) x = x << n;
    return x;
}
NI static uint32_t guarded_shr(uint32_t x, unsigned n)
{
    if (n < 32) x >>= n;
    else x = 0;
    return x;
}

/* Two steps in the arm. */
NI static int twostep(int c, int a, int b, int k)
{
    int r = a;
    if (c > k) r = (a + b) ^ k;
    return r;
}

/* The arm reads what it writes. */
NI static int accum(const int *v, int n, int lim)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        if (v[i] < lim) s = s + v[i];
    return s;
}

/* An arm that updates one value and copies it to another: the update
 * may not move ahead of the test, where it would run on both paths. */
NI static int pair(int c, int a)
{
    int b = 0;
    for (int i = 0; i < 3; i++)
        if (c > i) { a = a + 5; b = a; }
    return a * 100 + b;
}

/* A division is never run on both paths: by zero it traps on x86-64. */
NI static int guarded_div(int x, int d)
{
    int q = -1;
    if (d != 0) q = x / d + 1;
    return q;
}

NI static uint64_t wide(int c, uint64_t a, uint64_t b)
{
    uint64_t r = a;
    if (c) r = a + b;
    return r;
}

int main(void)
{
    int bad = 0;
    static const char t[] = "The Quick, Brown FOX; 42 jumps 7 Times";
    if (upper(t) != 7) bad |= 1;            /* T Q B F O X T */
    unsigned o[3];
    counts(t, o);
    if (o[0] != 6 || o[1] != 7 || o[2] != (unsigned)(',' ^ ';')) bad |= 2;
    if (addsub(1, 5, 3) != 8 || addsub(0, 5, 3) != 2) bad |= 4;
    for (unsigned n = 0; n < 70; n += 3) {
        uint32_t x = 0x80000001u;
        uint32_t want = n < 32 ? x << n : x;
        if (guarded_shl(x, n) != want) bad |= 8;
        if (guarded_shr(x, n) != (n < 32 ? x >> n : 0)) bad |= 16;
    }
    if (twostep(5, 10, 3, 2) != (13 ^ 2) || twostep(1, 10, 3, 2) != 10)
        bad |= 32;
    int v[8] = { 5, -3, 100, 7, 64, 9, -50, 63 };
    if (accum(v, 8, 64) != 5 - 3 + 7 + 9 - 50 + 63) bad |= 64;
    if (pair(0, 1) != 100 || pair(2, 1) != 1100 + 11) bad |= 512;
    if (guarded_div(7, 0) != -1 || guarded_div(7, 2) != 4) bad |= 256;
    if (wide(1, 0xffffffffull, 1) != 0x100000000ull ||
        wide(0, 0xffffffffull, 1) != 0xffffffffull) bad |= 128;
    return bad ? 1 + bad % 41 : 42;
}
