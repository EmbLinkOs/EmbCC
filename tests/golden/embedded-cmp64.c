/* 64-bit comparisons with a constant on a 32-bit machine.
 *
 * Thumb lowers `x OP K` without building K wherever K's halves are
 * immediates: `subs; sbcs` of x - K (with `x > K` as `x >= K + 1`),
 * `rsbs; mvn; adcs` of K - x when K + 1 does not encode, and `cmp lo;
 * it eq; cmpeq hi` for equality. The constants below sit on each side of
 * where those forms change -- K + 1 crossing a word, the type's maximum
 * and minimum, a half that is a modified immediate and one that is not
 * -- and every one is compared with K - 1, K and K + 1 and the extremes,
 * signed and unsigned, as a value and as a branch.
 *
 * The expected answers are not written down here: the same source runs
 * on the host, and its output is the reference.
 */
extern void writec(int c);
extern void puts_(const char *s);
extern void putn(long v);

#define NI __attribute__((noinline))

#define KS(X) X(0, 0LL) X(1, 1LL) X(2, 1000LL) X(3, -5LL) X(4, -1LL) \
    X(5, 0x7fffffffLL) X(6, 0x80000000LL) X(7, 0xffffffffLL)            \
    X(8, 0x100000000LL) X(9, 0x80000001LL) X(10, 0xff00000000LL)        \
    X(11, 0x7fffffffffffffffLL) X(12, -0x7fffffffffffffffLL - 1)        \
    X(13, 0x3e800000000LL) X(14, -0x100000000LL) X(15, 255LL)

#define VAL(n, K)                                                       \
    static NI int sv##n(long long x)                                    \
    {                                                                   \
        return (x < K) | (x <= K) << 1 | (x > K) << 2 | (x >= K) << 3 | \
               (x == K) << 4 | (x != K) << 5;                           \
    }                                                                   \
    static NI int uv##n(unsigned long long x)                           \
    {                                                                   \
        const unsigned long long k = (unsigned long long)(K);          \
        return (x < k) | (x <= k) << 1 | (x > k) << 2 | (x >= k) << 3 | \
               (x == k) << 4 | (x != k) << 5;                           \
    }                                                                   \
    static NI int sb##n(long long x)                                    \
    {                                                                   \
        int r = 0;                                                      \
        if (x < K) r |= 1;                                              \
        if (x <= K) r |= 2;                                             \
        if (x > K) r |= 4;                                              \
        if (x >= K) r |= 8;                                             \
        if (x == K) r |= 16;                                            \
        if (x != K) r |= 32;                                            \
        return r;                                                       \
    }                                                                   \
    static NI int ub##n(unsigned long long x)                           \
    {                                                                   \
        const unsigned long long k = (unsigned long long)(K);          \
        int r = 0;                                                      \
        if (x < k) r |= 1;                                              \
        if (x <= k) r |= 2;                                             \
        if (x > k) r |= 4;                                              \
        if (x >= k) r |= 8;                                             \
        if (x == k) r |= 16;                                            \
        if (x != k) r |= 32;                                            \
        return r;                                                       \
    }
KS(VAL)

/* The same six with K in a register: the swapped operands of `>` and
 * `<=`, and equality of two pairs. */
static NI int sr(long long x, long long k)
{
    return (x < k) | (x <= k) << 1 | (x > k) << 2 | (x >= k) << 3 |
           (x == k) << 4 | (x != k) << 5;
}
static NI int ur(unsigned long long x, unsigned long long k)
{
    return (x < k) | (x <= k) << 1 | (x > k) << 2 | (x >= k) << 3 |
           (x == k) << 4 | (x != k) << 5;
}

static const long long ks[] = {
#define KV(n, K) K,
    KS(KV)
};
static long long extra[] = {
    0, 1, -1, 0x7fffffffffffffffLL, -0x7fffffffffffffffLL - 1,
    0x80000000LL, 0xffffffffLL, 0x100000000LL, 0x7fffffffLL, -0x80000000LL,
};

static void hx2(int v)
{
    writec("0123456789abcdef"[(v >> 4) & 3]);
    writec("0123456789abcdef"[v & 15]);
}

static void one(int n, long long x)
{
    int s, u, b, c;
    switch (n) {
#define CASE(n, K) case n: s = sv##n(x); u = uv##n((unsigned long long)x); \
        b = sb##n(x); c = ub##n((unsigned long long)x); break;
    KS(CASE)
    default: s = u = b = c = 0; break;
    }
    hx2(s); hx2(u);
    /* The branch forms must say what the value forms say. */
    if (b != s || c != u) writec('!');
    if (sr(x, ks[n]) != s || ur((unsigned long long)x,
                                (unsigned long long)ks[n]) != u)
        writec('?');
    writec(' ');
}

int main(void)
{
    int nk = (int)(sizeof ks / sizeof ks[0]);
    int ne = (int)(sizeof extra / sizeof extra[0]);
    for (int n = 0; n < nk; n++) {
        long long k = ks[n];
        /* K - 1 and K + 1 wrap at the extremes, which is what is
         * wanted: both ends get compared with both extremes. */
        long long km = (long long)((unsigned long long)k - 1);
        long long kp = (long long)((unsigned long long)k + 1);
        putn(n);
        writec(':');
        one(n, km); one(n, k); one(n, kp);
        for (int e = 0; e < ne; e++)
            one(n, extra[e]);
        writec('\n');
    }
    puts_("==END==\n");
    return 0;
}
