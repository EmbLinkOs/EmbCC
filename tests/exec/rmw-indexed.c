// expect-exit: 42
/* `a[i - 1] op= x` on x86-64 is one read-modify-write instruction whose
 * memory operand carries the index, scaled -- `add %x, (%base,%i,4)` --
 * where it was a sign extension, a shift, an add and the RMW. Each
 * operation, from a register and from an immediate, at four and eight
 * bytes; and enough live values in `crowd` that the allocator reaches
 * r8-r15 for the base and the index, whose encodings need the REX bits.
 * `stack` and `wide` read the right-hand side from memory AFTER the
 * address is computed and before the load through it, the shape of a
 * stack machine's `s[sp - 1] += s[sp]`: the address is held, not
 * computed, across those loads. And it may not be held where it cannot
 * be: in `reuse` the base and the index die at the address and the
 * allocator hands their registers to the right-hand side; in `shifty` the
 * index arrives in rcx, which a variable shift takes; `across` calls
 * between the load and the operation. */
typedef unsigned u32;
typedef unsigned long long u64;

__attribute__((noinline)) static void ops(u32 *s, int sp, u32 x)
{
    s[sp - 1] += x;
    s[sp - 2] -= x;
    s[sp - 3] &= x;
    s[sp - 4] |= x;
    s[sp - 5] ^= x;
}

__attribute__((noinline)) static void imms(u64 *s, long i)
{
    s[i] += 1000;
    s[i + 1] -= 7;
    s[i + 2] &= 0xff00ff00ff00ff00ull;
    s[i + 3] |= 0x40;
    s[i + 4] ^= 0x123456789ull;
}

__attribute__((noinline)) static u32 crowd(u32 *a, u32 *b, int i, int j,
                                           u32 x, u32 y, u32 z, u32 w)
{
    u32 k = x * 3 + y, m = y * 5 + z, n = z * 7 + w, q = w * 11 + x;
    a[i] += k;
    b[j] ^= m;
    a[j] -= n;
    b[i] |= q;
    return k + m + n + q;
}

__attribute__((noinline)) static u32 stack(u32 *s, const unsigned char *code,
                                           int n)
{
    int sp = 1;
    for (int pc = 0; pc < n; pc++) {
        switch (code[pc]) {
        case 0: s[sp - 1] += s[sp]; sp--; break;
        case 1: s[sp - 1] -= s[sp]; sp--; break;
        case 2: s[sp - 1] &= s[sp]; sp--; break;
        case 3: s[sp - 1] |= s[sp]; sp--; break;
        case 4: s[sp - 1] ^= s[sp]; sp--; break;
        default: sp++; s[sp] = code[pc] * 2654435761u + (u32)sp; break;
        }
    }
    return s[sp] + (u32)sp * 1000u;
}

__attribute__((noinline)) static void wide(u64 *t, long i, long j, long k,
                                           const unsigned char *c)
{
    t[i] += t[j] + t[k];
    t[j] -= t[c[i]];
    t[k] ^= (u64)c[j] << 40;
}

__attribute__((noinline)) static void reuse(u32 *a, long i, const u32 *b,
                                            const u32 *c, long j)
{
    a[i] += b[j] + c[j];
}

__attribute__((noinline)) static void reuse2(u64 *a, int i, const u64 *b,
                                             int j, int k)
{
    a[i] -= b[j] ^ b[k];
}

__attribute__((noinline)) static void shifty(u32 *a, int s, const u32 *b,
                                             long k)
{
    a[k] += b[1] << s;
}

__attribute__((noinline)) static u32 twice(u32 x)
{
    return x * 2;
}

__attribute__((noinline)) static void across(u32 *a, long i, u32 x)
{
    a[i] += twice(x);
}

int main(void)
{
    u32 s[8] = { 100, 200, 300, 400, 500, 600, 700, 800 };
    u64 t[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    u32 a[4] = { 1, 2, 3, 4 }, b[4] = { 10, 20, 30, 40 };
    ops(s, 7, 0x0f0f);
    /* s[6] += x, s[5] -= x, s[4] &= x, s[3] |= x, s[2] ^= x */
    if (s[6] != 700 + 0x0f0f || s[5] != 600 - 0x0f0f || s[4] != (500 & 0x0f0f) ||
        s[3] != (400 | 0x0f0f) || s[2] != (300 ^ 0x0f0f) || s[1] != 200)
        return 1;
    imms(t, 2);
    if (t[2] != 1003 || t[3] != 4 - 7 || t[4] != (5 & 0xff00ff00ff00ff00ull) ||
        t[5] != (6 | 0x40) || t[6] != (7 ^ 0x123456789ull) || t[7] != 8)
        return 2;
    if (crowd(a, b, 1, 3, 2, 3, 4, 5) != 9 + 19 + 33 + 57) return 3;
    if (a[1] != 2 + 9 || b[3] != (40 ^ 19) || a[3] != 4 - 33 || b[1] != (20 | 57))
        return 4;
    {
        u32 st[16] = { 7, 11 };
        static const unsigned char code[] = { 9, 0, 200, 1, 77, 13, 2, 3, 5,
                                              4, 99, 0 };
        u64 w[6] = { 0x1111, 0x22222222, 0x333333333ull, 4, 5, 6 };
        static const unsigned char c[] = { 3, 5, 0, 1, 2, 4 };
        if (stack(st, code, (int)sizeof code) != 318534120u) return 5;
        if (st[0] != 7 || st[1] != 318533120u || st[2] != 796135285u ||
            st[3] != 147926528u || st[4] != 0)
            return 6;
        wide(w, 1, 3, 4, c);
        if (w[0] != 0x1111 || w[1] != 0x2222222bull || w[2] != 0x333333333ull ||
            w[3] != 0xfffffffffffffffeull || w[4] != 0x10000000005ull ||
            w[5] != 6)
            return 7;
    }
    {
        u32 a[4] = { 1, 2, 3, 4 }, b[4] = { 10, 20, 30, 40 };
        u32 c[4] = { 5, 6, 7, 8 };
        u64 d[4] = { 100, 200, 300, 400 }, e[4] = { 1, 2, 4, 8 };
        reuse(a, 2, b, c, 1);
        if (a[2] != 3 + 20 + 6 || a[1] != 2 || a[3] != 4) return 8;
        reuse2(d, 3, e, 1, 2);
        if (d[3] != 400 - (2 ^ 4) || d[2] != 300) return 9;
        shifty(a, 3, b, 1);
        if (a[1] != 2 + (20u << 3) || a[0] != 1 || a[2] != 29) return 10;
        across(a, 3, 50);
        if (a[3] != 104 || a[2] != 29) return 11;
    }
    return 42;
}
