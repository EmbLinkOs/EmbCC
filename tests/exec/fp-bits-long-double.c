// expect-exit: 42
/* The sign and classification builtins on a 16-byte long double --
 * x87 80-bit on x86-64, binary128 on AArch64 -- which were refused: the
 * sign and exponent are past one register, so they go through memory. */
#include <float.h>
__attribute__((noinline)) static long double id(long double x) { return x; }

int main(void)
{
    volatile long double zero = 0.0L;
    long double nz = id(-0.0L), big = id(LDBL_MAX), tiny = id(LDBL_MIN / 4);
    long double inf = id(1.0L / zero), nan = id(zero / zero);
    if (__builtin_signbit(__builtin_fabsl(nz))) return 1;
    if (!__builtin_signbit(nz) || __builtin_signbit(big)) return 2;
    if (__builtin_copysignl(3.0L, -1.0L) != -3.0L) return 3;
    if (__builtin_signbit(__builtin_copysignl(nz, 1.0L))) return 4;
    if (__builtin_fabsl(id(-big)) != big) return 5;
    if (__builtin_isinf(big) || !__builtin_isfinite(big)) return 6;
    if (!__builtin_isinf(inf) || __builtin_isinf_sign(-inf) != -1) return 7;
    if (__builtin_isinf_sign(inf) != 1 || __builtin_isinf_sign(big) != 0) return 8;
    if (!__builtin_isnan(nan) || __builtin_isnan(inf) || __builtin_isnan(big)) return 9;
    if (__builtin_isnormal(tiny) || !__builtin_isnormal(big)) return 10;
    if (__builtin_isnormal(zero) || __builtin_isnormal(inf)) return 11;
    if (__builtin_isfinite(nan) || __builtin_isfinite(inf)) return 12;
    if (!(__builtin_fabsl(id(-1e300L * 1e300L)) > 1e300L)) return 13;   /* beyond double */
    return 42;
}
