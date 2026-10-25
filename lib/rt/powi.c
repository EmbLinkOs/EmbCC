/* GCC's __builtin_powi, __builtin_powif and __builtin_powil: x to an
 * integer power, under libgcc's names (src/ir/irgen.c calls these), so an
 * object of GCC's that calls them links here too.
 *
 *   float       __powisf2(float x, int n)
 *   double      __powidf2(double x, int n)
 *   long double __powixf2 (x87's, x86-64) or __powitf2 (binary128),
 *               where long double is wider than double
 *
 * libgcc's algorithm, square and multiply over n's bits, and 1 / x^-n for
 * a negative n -- not pow(): GCC documents no precision for powi, and
 * a program comparing against GCC gets GCC's rounding. */

#define POWI(NAME, T)                                                     \
    T NAME(T x, int n);                                                   \
    T NAME(T x, int n)                                                    \
    {                                                                     \
        unsigned m = n < 0 ? -(unsigned)n : (unsigned)n;                  \
        T r = (m & 1) ? x : (T)1;                                         \
        while (m >>= 1) {                                                 \
            x = x * x;                                                    \
            if (m & 1)                                                    \
                r = r * x;                                                \
        }                                                                 \
        return n < 0 ? (T)1 / r : r;                                      \
    }

POWI(__powisf2, float)
POWI(__powidf2, double)
#if __SIZEOF_LONG_DOUBLE__ > __SIZEOF_DOUBLE__
#if defined(__x86_64__)
POWI(__powixf2, long double)
#else
POWI(__powitf2, long double)
#endif
#endif
