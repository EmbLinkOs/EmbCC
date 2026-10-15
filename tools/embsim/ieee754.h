/* ieee754.h -- IEEE 754 arithmetic in integers (ieee754.c), which the
 * FPUs share: RISC-V's F and D (riscv-fpu.c) and a MIPS FPU (mips-fpu.c).
 * `d` is 0 for binary32, 1 for binary64; a value is its bits, in a u64.
 * `rm` is one of the five rounding modes; each operation ors the
 * exceptions it raised into *fl. */
#ifndef EMBSIM_IEEE754_H
#define EMBSIM_IEEE754_H

#include "sim.h"

enum { IEEE_RNE, IEEE_RTZ, IEEE_RDN, IEEE_RUP, IEEE_RMM };

/* the exception flags, as RISC-V's fflags orders them */
#define IEEE_NX 1u                  /* inexact */
#define IEEE_UF 2u                  /* underflow */
#define IEEE_OF 4u                  /* overflow */
#define IEEE_DZ 8u                  /* division by zero */
#define IEEE_NV 16u                 /* invalid */

u64 ieee_add(int d, u64 a, u64 b, int sub, int rm, u32 *fl);
u64 ieee_mul(int d, u64 a, u64 b, int rm, u32 *fl);
/* a * b + c, rounded once; negp negates the product, negc c */
u64 ieee_fma(int d, u64 a, u64 b, u64 c, int negp, int negc, int rm, u32 *fl);
u64 ieee_div(int d, u64 a, u64 b, int rm, u32 *fl);
u64 ieee_sqrt(int d, u64 a, int rm, u32 *fl);
/* to an integer of `bits` (32 or 64), signed or not, saturating (a NaN
 * gives the largest positive) */
u64 ieee_to_int(int d, u64 a, int bits, int uns, int rm, u32 *fl);
u64 ieee_from_int(int d, u64 v, int bits, int uns, int rm, u32 *fl);
/* binary32 to binary64 (to_d 1, exact) or back (rounded) */
u64 ieee_convert(int to_d, u64 a, int rm, u32 *fl);
/* op 0: a <= b, 1: a < b (both signalling), 2: a == b (quiet) */
int ieee_cmp(int d, u64 a, u64 b, int op, u32 *fl);
/* minimumNumber and maximumNumber */
u64 ieee_minmax(int d, u64 a, u64 b, int max, u32 *fl);
/* RISC-V's FCLASS bits */
u64 ieee_class(int d, u64 a);
int ieee_isnan(int d, u64 a);
int ieee_issnan(int d, u64 a);
u64 ieee_default_nan(int d);

#endif
