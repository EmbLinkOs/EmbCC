/* The SVR4/EABI shapes tests/golden/ppc-abi-{caller,callee}.c pass between
 * them: one is compiled by EmbCC and the other by clang
 * (--target=powerpc-none-eabi -mcpu=e500 -mno-spe -msoft-float
 * -mlong-double-64), and every pairing must print the same thing
 * (tests/golden/ppc-abi.sh).
 *
 * What is particular to the PowerPC EABI (docs/internals/powerpc-plan.md),
 * and so what this exercises beyond embedded-abi.h and mips-abi.h:
 *   - an 8-byte scalar takes an ODD-numbered pair, high word first, so
 *     after one int a long long skips r4 for r5:r6, and after seven ints
 *     it does not fit r10 and goes to the stack, 8-aligned at 8(r1) --
 *     and every later argument follows it there, r10 left empty;
 *   - EVERY composite is passed by reference to the caller's copy: the
 *     callee may write it, and the caller's own must not change;
 *   - a composite of 8 bytes or fewer comes back in r3:r4 right-
 *     justified (1, 2, 3, 5, 6, 7 and 8 bytes), a larger one through r3;
 *     a _Complex float in r3:r4, a _Complex double through r3;
 *   - variadic: the caller clears CR bit 6; a va_list is the 12-byte
 *     record, which a v-function compiled by the OTHER compiler walks;
 *     va_arg of a long long rounds the register count to even, a struct
 *     arrives as a pointer;
 *   - long double is double.
 */
#include <stdarg.h>
struct s1 { unsigned char a; };
struct s2 { unsigned char a, b; };
struct s3 { unsigned char a, b, c; };
struct s5 { unsigned char c[5]; };
struct s6 { short h[3]; };
struct s7 { unsigned char c[7]; };
struct s8 { int a, b; };
struct s12 { int a, b, c; };
struct mod { int v[5]; };

long long ll_after_1(int a, long long b);
long long ll_after_7(int a, int b, int c, int d, int e, int f, int g,
                     long long h, int i);
long long ll_after_6(int a, int b, int c, int d, int e, int f, long long g,
                     int h);
int ints_10(int a, int b, int c, int d, int e, int f, int g, int h, int i,
            int j);
double d_after_7(int a, int b, int c, int d, int e, int f, int g, double h,
                 float i, double j);
long double ld_add(long double a, long double b);
int take_mod(struct mod m, int k);
int take_s3_s8(struct s3 a, struct s8 b, int k);
int structs_late(int a, int b, int c, int d, int e, int f, int g, int h,
                 struct s12 s, struct s1 t);
struct s1 ret_s1(int k);
struct s2 ret_s2(int k);
struct s3 ret_s3(int k);
struct s5 ret_s5(int k);
struct s6 ret_s6(int k);
struct s7 ret_s7(int k);
struct s8 ret_s8(int k);
struct s12 ret_s12(int k);
_Complex float ret_cf(float re, float im);
_Complex double ret_cd(double re, double im);
double take_cd(_Complex double z, _Complex float w);
long long vll_late(int n, ...);
int vstructs(int n, ...);
double vfloat(int n, ...);
int vlist_sum(int n, va_list ap);
int vlist_call(int n, ...);
int vlist_other(int n, ...);
