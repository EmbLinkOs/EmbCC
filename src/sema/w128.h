/* 128-bit integer arithmetic on two eightbytes, for folding __int128
 * constants (sema's static initializers, C++'s constant expressions).
 * EmbCC compiles itself, so it cannot lean on an __int128 of its own. */
#ifndef EMBCC_SEMA_W128_H
#define EMBCC_SEMA_W128_H

struct w128 {
    unsigned long lo, hi;
};

struct w128 w_make(unsigned long lo, unsigned long hi);
/* v extended to 128 bits: sign-extended if sign, else zero-extended */
struct w128 w_from(long v, int sign);
struct w128 w_add(struct w128 a, struct w128 b);
struct w128 w_neg(struct w128 a);
struct w128 w_mul(struct w128 a, struct w128 b);
struct w128 w_shl(struct w128 a, int n);
struct w128 w_shr(struct w128 a, int n, int arith);
int w_ult(struct w128 a, struct w128 b);
int w_slt(struct w128 a, struct w128 b);
/* unsigned a / b and a % b (b nonzero) */
void w_udivmod(struct w128 a, struct w128 b, struct w128 *q, struct w128 *r);
/* signed or unsigned a / b, a % b; 0 if b is zero */
int w_div(struct w128 a, struct w128 b, int sign, int mod, struct w128 *out);

#endif
