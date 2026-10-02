// expect-exit: 42
/* An enumerator whose value int cannot hold. The enum then has the type
 * GCC and clang give it -- unsigned int, then long, unsigned long, long
 * long -- and so do its enumerators (C23 6.7.2.2), as with a fixed
 * underlying type (`enum e : T`). Every enumerator was int whatever its
 * value: `enum { G = 0x100000005 }` read as 5 in any expression the
 * compiler did not fold at file scope, and the enum was 4 bytes against
 * their 8. */
enum wide { G = 0x100000005 };
enum fixed : unsigned long long { X = 0x100000000ULL, Y };
enum uns { U = 0xFFFFFFFFu };
enum mixed { MN = -1, MX = 0x80000000 };
enum small { S1 = -5, S2 = 7 };
#define T(x) _Generic((x), int: 1, unsigned: 2, long: 3, unsigned long: 4, \
                      long long: 5, unsigned long long: 6, default: 0)

__attribute__((noinline)) static long long g(void) { return G; }
__attribute__((noinline)) static long long x(void) { return X; }
__attribute__((noinline)) static long long y(void) { return Y; }
__attribute__((noinline)) static int uneg(void) { return U > 0; }
__attribute__((noinline)) static long long mx(void) { return MX; }

int main(void)
{
    enum wide w = G;
    if (g() != 0x100000005LL || w != G) return 1;
    if (x() != 0x100000000LL || y() != 0x100000001LL) return 2;
    if (sizeof(enum wide) != 8 || sizeof(enum fixed) != 8) return 3;
    if (sizeof(enum uns) != 4 || T(U) != 2 || !uneg()) return 4;
    if (sizeof(enum mixed) != sizeof(long) || mx() != 0x80000000LL) return 5;
    if (sizeof(enum small) != 4 || T(S1) != 1) return 6;
    if (T(X) != 6 && T(X) != 4) return 7;
    /* no negative value: an unsigned type, which the tag keeps too */
    {
        enum uns u2 = U;
        if (u2 < 0 || T(u2) != 2) return 8;
        if (T(G) != 4) return 9;                /* unsigned long */
        if (T(MN) != 3) return 10;              /* long: it has -1 */
    }
    return 42;
}
