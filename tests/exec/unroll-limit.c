/* The unrolled loop's limit, at the ends of the counter's range. The
 * unroller runs its copies while iv <= bound - U*step, computed once on
 * the way in; that limit is sound only if the subtraction did not wrap,
 * and a loop that fails the check must still run, as the original. So:
 * signed loops ending just above INT_MIN and LLONG_MIN (the limit wraps
 * to a large positive), loops ending at INT_MAX (the counter reaches the
 * top), `!=` loops whose end is below U*step and ones that wrap through
 * zero, at 32 and 64 bits and with steps of 1 and 4, every trip count
 * from 0 past several runs of copies. Expectations are closed forms. */
// expect-exit: 42
typedef unsigned long long u64;
#define IMAX 2147483647
#define IMIN (-IMAX - 1)
#define LMAX 9223372036854775807LL
#define LMIN (-LMAX - 1)

__attribute__((noinline)) long long up_int(int lo, int hi, int *cnt)
{ long long s = 0; int c = 0; for (int i = lo; i < hi; i++) { s += i; c++; } *cnt = c; return s; }
__attribute__((noinline)) u64 up_ll(long long lo, long long hi, int *cnt)
{ u64 s = 0; int c = 0; for (long long i = lo; i < hi; i++) { s += (u64)i; c++; } *cnt = c; return s; }
__attribute__((noinline)) u64 ne1(unsigned from, unsigned to, int *cnt)
{ u64 s = 0; int c = 0; for (unsigned i = from; i != to; i++) { s += i; c++; } *cnt = c; return s; }
__attribute__((noinline)) u64 ne4(unsigned from, unsigned to, int *cnt)
{ u64 s = 0; int c = 0; for (unsigned i = from; i != to; i += 4) { s += i; c++; } *cnt = c; return s; }
__attribute__((noinline)) u64 ne1_ll(u64 from, u64 to, int *cnt)
{ u64 s = 0; int c = 0; for (u64 i = from; i != to; i++) { s += i; c++; } *cnt = c; return s; }

/* sum of n terms from a by d, each taken mod 2^32 (the counter's type) */
static u64 sum32(unsigned a, u64 n, unsigned d)
{
    u64 m = (0x100000000ULL - a + d - 1) / d;    /* terms before the wrap */
    u64 s = n * a + d * (n * (n - 1) / 2);
    return n > m ? s - ((n - m) << 32) : s;
}

static const int trips[] = { 0, 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 33, 64, 65 };
#define NT (int)(sizeof trips / sizeof *trips)

/* Each failure returns at once with its own code: a limit too high
 * sends a `!=` loop past its end and round the whole range, so the
 * value checks come first, then the `!=` loops, and last the loops
 * whose limit wraps -- unchecked, those run (almost) forever, and a
 * timeout is the only way they can fail. */
int main(void)
{
    int c;
    for (int k = 0; k < NT; k++) {
        int n = trips[k];
        long long nl = n;
        /* signed, 32-bit: around zero, up to INT_MAX */
        static const long long los[] = { -40, -5, 0, IMAX };
        for (int j = 0; j < 4; j++) {
            long long lo = j == 3 ? IMAX - nl : los[j];
            long long want = nl * lo + nl * (nl - 1) / 2;
            if (up_int((int)lo, (int)(lo + nl), &c) != want || c != n) return 1;
            if (n && (up_int((int)(lo + nl), (int)lo, &c) != 0 || c != 0)) return 2;
        }
        /* signed, 64-bit: the same */
        static const long long lls[] = { -40, 0, LMAX };
        for (int j = 0; j < 3; j++) {
            long long lo = j == 2 ? LMAX - nl : lls[j];
            u64 want = (u64)nl * (u64)lo + (u64)(nl * (nl - 1) / 2);
            if (up_ll(lo, lo + nl, &c) != want || c != n) return 3;
        }
    }
    for (int k = 0; k < NT; k++) {
        int n = trips[k];
        /* `!=`, 32-bit: ends below U*step, and walks through zero */
        static const unsigned tos[] = { 0, 1, 3, 7, 8, 31, 32, 0x80000000u, 0xFFFFFFFFu };
        for (int j = 0; j < 9; j++) {
            unsigned to = tos[j], from = to - (unsigned)n;
            if (ne1(from, to, &c) != sum32(from, (u64)n, 1) || c != n) return 4;
            unsigned from4 = to - 4u * (unsigned)n;
            if (ne4(from4, to, &c) != sum32(from4, (u64)n, 4) || c != n) return 5;
        }
        /* `!=`, 64-bit, through zero: the sum is modular either way */
        static const u64 tls[] = { 0, 5, 0x8000000000000000ULL, ~0ULL };
        for (int j = 0; j < 4; j++) {
            u64 to = tls[j], from = to - (u64)n;
            u64 want = (u64)n * from + (u64)n * (u64)(n - 1) / 2;
            if (ne1_ll(from, to, &c) != want || c != n) return 6;
        }
    }
    for (int k = 0; k < NT; k++) {
        long long nl = trips[k];
        /* signed, ending just above the minimum: bound - U*step wraps */
        if (up_int(IMIN, (int)(IMIN + nl), &c) != nl * IMIN + nl * (nl - 1) / 2 || c != nl)
            return 7;
        if (up_ll(LMIN, LMIN + nl, &c) != (u64)nl * (u64)LMIN + (u64)(nl * (nl - 1) / 2) || c != nl)
            return 8;
    }
    return 42;
}
