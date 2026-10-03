// expect-exit: 42
/* Enumerators past LLONG_MAX, and the names enumerators share with
 * locals.
 *
 * An enumerator was held as a long, so 1ULL << 63 read as LLONG_MIN:
 * the enum became a signed long long, `B > 0` was 0 and `B >> 62` was
 * -2. gcc and clang make it an unsigned long.
 *
 * In an array bound, an enumerator was found before a local or a
 * parameter of the same name: with `int M = 5;` in scope, `int a[M]`
 * had the file's `enum { M = 2 }` elements, and sizeof said 8, not 20.
 * Neither came with a diagnostic. */
enum big { B = 1ULL << 63, C = 1 };
enum top { T = 0xffffffffffffffffULL };
enum neg_size { NS = -sizeof(char) };
enum { M = 2 };

_Static_assert(sizeof(enum big) == 8, "enum big is 64-bit");
_Static_assert(B > 0, "2^63 is positive");
_Static_assert((B >> 62) == 2, "and shifts as unsigned");
_Static_assert(T > B, "ULLONG_MAX");
_Static_assert(NS > 0, "-sizeof is a size_t");
_Static_assert(sizeof(M) == sizeof(int), "sizeof an enumerator folds");
_Static_assert(_Generic(1L, long long: 0, long: 1), "strict _Generic fold");

__attribute__((noinline)) static int cmp(enum big v) { return v > C; }
__attribute__((noinline)) static long long shr(enum big v) { return (long long)(v >> 62); }

__attribute__((noinline)) static int param(int M) { int a[M]; return (int)sizeof a; }

__attribute__((noinline)) static int local(void)
{
    int M = 5;
    int a[M];
    return (int)sizeof a;
}

__attribute__((noinline)) static int scopes(void)
{
    int r = 0;
    {
        int M = 7;
        int a[M];
        r = (int)sizeof a;               /* 28: the local */
    }
    for (int M = 0; M < 1; M++)
        r += M;
    int b[M];                            /* 8: the enumerator again */
    return r * 100 + (int)sizeof b;
}

int main(void)
{
    if (!cmp(B)) return 1;
    if (shr(B) != 2) return 2;
    if (!(T > 0) || T != ~0ULL) return 3;
    if (param(3) != 12) return 4;
    if (local() != 20) return 5;
    if (scopes() != 2808) return 6;
    return 42;
}
