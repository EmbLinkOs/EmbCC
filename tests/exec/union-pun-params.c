/* Type punning through a union, of PARAMETERS: `u.l = x; return u.d;`.
 * The optimizer folds the union away and returns the integer parameter
 * itself as a double, so a value that arrived in an integer register is
 * read as floating point with nothing in between. On x86-64 at -O1 and
 * up that was an internal compiler error ("a frame access in a function
 * that has no frame pointer"): the value had been put in the float class,
 * where an integer parameter had no home the prologue could fill. The
 * integers and doubles are interleaved so the argument registers of both
 * classes are shuffled at entry.
 */
// expect-exit: 42
#include <stdint.h>

__attribute__((noinline)) static double bits(int64_t x)
{ union { int64_t l; double d; } u; u.l = x; return u.d; }

__attribute__((noinline)) static float fbits(int32_t x)
{ union { int32_t i; float f; } u; u.i = x; return u.f; }

__attribute__((noinline)) static int64_t unbits(double d)
{ union { int64_t l; double d; } u; u.d = d; return u.l; }

__attribute__((noinline)) static double twice(int64_t x)
{ union { int64_t l; double d; } u; u.l = x; return u.d * 2.0; }

__attribute__((noinline)) static double mix(int64_t a, double b, int64_t c, float e)
{
    union { int64_t l; double d; } u, v;
    u.l = a; v.l = c;
    return u.d + b + v.d + e;
}

int main(void)
{
    if (bits(0x4000000000000000LL) != 2.0) return 1;
    if (fbits(0x40400000) != 3.0f) return 2;
    if (unbits(1.5) != 0x3ff8000000000000LL) return 3;
    if (twice(0x3ff0000000000000LL) != 2.0) return 4;          /* 1.0 * 2 */
    if (mix(0x3ff0000000000000LL, 0.5, 0x4000000000000000LL, 0.25f) != 3.75)
        return 5;
    if (unbits(bits(0x123456789abcdef0LL)) != 0x123456789abcdef0LL) return 6;
    return 42;
}
