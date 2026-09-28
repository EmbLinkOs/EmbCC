/* The AVR compiler runtime: multiply, divide and remainder.
 *
 * AVR has no divide instruction at all, and its multiply is 8x8 -> 16 into
 * r1:r0 -- which DESTROYS r1, the machine's zero register that every other
 * lowering in the backend reads. A 32-bit product inline is ten partial
 * products with a carry chain threaded through them, about seventy
 * instructions, so 140 bytes of flash at every site on a part that has
 * 32 KB. Every AVR toolchain calls a helper instead, and so does this one.
 *
 * ---- the constraint, which rt.h states and this file lives under -------
 *
 * These routines may not use the operations they implement. `__mulsi3`
 * cannot multiply, because that is a call to `__mulsi3`. So everything
 * here is shifts, adds and comparisons -- and even the shifts are written
 * as `x += x` rather than `x <<= 1`, because an add is one instruction per
 * byte on this machine where the backend's shift-by-one is the same thing
 * with an extra step. `x >>= 1` by a CONSTANT is fine; a shift by a
 * VARIABLE is a count-down loop on a machine with no barrel shifter, so
 * nothing here writes one.
 *
 * ---- the width, and why one set is enough ------------------------------
 *
 * Only the 32-bit forms. The backend computes every value at four bytes,
 * because EmbIR's width class is 4, 8 or 16 and never 2 -- so a two-byte
 * `int` reaches codegen already extended to four, and a 32-bit divide of
 * two sign-extended 16-bit values has the right low 16 bits. avr-gcc
 * carries __divmodhi4 and an inline 16-bit multiply as well and is faster
 * for `int` arithmetic because of it; catching up means teaching the
 * backend a per-value width, not adding narrower helpers here.
 *
 * ---- the names ---------------------------------------------------------
 *
 * libgcc's, and single-result. avr-gcc's own code calls __divmodsi4, which
 * returns the quotient in r18-r21 AND the remainder in r22-r25 -- two
 * results in a register layout no C function can express. Matching it
 * needs assembly, and EmbCC's assembler has no AVR support yet, so that is
 * recorded as an open item rather than approximated. Nothing is lost for
 * interoperation: these are extra symbols in EmbCC's own runtime, not
 * redefinitions of avr-libgcc's, so an EmbCC object and an avr-gcc object
 * in one program each resolve their own calls. The cost is that `a / b`
 * and `a % b` in one expression are two calls here where avr-gcc makes
 * one.
 *
 * Measured from clang -target avr, not recalled: see the commit that
 * added this file.
 *
 * The SIXTY-FOUR-bit helpers are in lib/rt/avr64.c, not here, and the split
 * is not tidiness: a runtime library is one routine per object precisely so
 * that a program pays only for what it calls. With both in one file an
 * ATmega328P program that never writes `long long` still carried
 * __muldi3, __udivdi3 and their friends -- and at -O0 that pushed
 * tests/golden/avr-exec's image to 34718 bytes on a part with 32768. The
 * symptom was `CALL 0x8770`, a call past the end of flash.
 */

/* This file is AVR's alone. lib/rt is built by a wildcard, so it has to
 * compile to nothing everywhere else rather than fail to compile. */
#ifdef __AVR__

typedef unsigned long u32_;
typedef long          s32_;

/* ---- multiply ---------------------------------------------------------
 *
 * Shift-and-add over the multiplier's bits. Signedness does not enter:
 * the low 32 bits of a two's-complement product are the same whether the
 * operands are read as signed or unsigned, which is why libgcc has one
 * __mulsi3 and not two.
 *
 * The loop runs until the multiplier is exhausted rather than a fixed 32
 * times, so a multiply by a small number costs what it should -- and the
 * common case on this machine, an array index scaled by a struct size,
 * the backend does inline with shifts and never reaches here.
 */
s32_ __mulsi3(s32_ a, s32_ b)
{
    u32_ x = (u32_)a, y = (u32_)b, r = 0;

    while (y) {
        if (y & 1)
            r += x;
        x += x;                  /* x <<= 1, as an add */
        y >>= 1;                 /* a CONSTANT shift: one step per byte */
    }
    return (s32_)r;
}

/* ---- unsigned divide and remainder ------------------------------------
 *
 * Restoring division, most significant bit first: 32 iterations, each one
 * shifting the next bit of the dividend into a remainder and subtracting
 * the divisor when it fits.
 *
 * The bit is taken with an AND against 0x80000000 rather than a shift by
 * 31, because a four-byte AND is four instructions and the backend's
 * constant shift by 31 is three byte moves and seven single-bit steps.
 * Every other shift here is an add. That is the whole reason this reads
 * the way it does instead of the textbook way.
 *
 * Divide by zero is undefined in C. It is given an answer anyway -- all
 * ones for the quotient, the dividend for the remainder, which is what
 * every hardware divider that traps nothing does -- because the
 * alternative on a bare part is an infinite loop inside a library routine
 * with no way to report it.
 */
static u32_ udivmod(u32_ n, u32_ d, u32_ *rem)
{
    u32_ q = 0, r = 0;
    int i;

    if (d == 0) {
        if (rem)
            *rem = n;
        return ~(u32_)0;
    }
    for (i = 0; i < 32; i++) {
        r += r;                              /* r <<= 1 */
        if (n & 0x80000000UL)
            r |= 1;
        n += n;                              /* n <<= 1 */
        q += q;                              /* q <<= 1 */
        if (r >= d) {
            r -= d;
            q |= 1;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

u32_ __udivsi3(u32_ a, u32_ b) { return udivmod(a, b, 0); }

u32_ __umodsi3(u32_ a, u32_ b)
{
    u32_ r;
    udivmod(a, b, &r);
    return r;
}

/* ---- signed divide and remainder --------------------------------------
 *
 * Magnitudes through the unsigned routine, signs applied afterwards. C99
 * requires truncation toward zero, which makes the quotient's sign the XOR
 * of the operands' and the remainder's sign the DIVIDEND's -- not the
 * divisor's, and not always positive. -7 / 2 is -3 and -7 % 2 is -1.
 *
 * The negations are on the UNSIGNED value. Negating the signed one is
 * undefined at LONG_MIN, and LONG_MIN is exactly the input a division
 * routine has to survive: its magnitude does not fit in a signed long, but
 * it fits perfectly in an unsigned one.
 */
static u32_ mag(s32_ v, int *neg)
{
    if (v < 0) {
        *neg = !*neg;
        return -(u32_)v;
    }
    return (u32_)v;
}

s32_ __divsi3(s32_ a, s32_ b)
{
    int neg = 0;
    u32_ q;
    u32_ ua = mag(a, &neg);
    u32_ ub = mag(b, &neg);

    q = udivmod(ua, ub, 0);
    return neg ? -(s32_)q : (s32_)q;
}

s32_ __modsi3(s32_ a, s32_ b)
{
    int dneg = 0, bneg = 0;
    u32_ r;
    u32_ ua = mag(a, &dneg);
    u32_ ub = mag(b, &bneg);

    udivmod(ua, ub, &r);
    /* The DIVIDEND's sign, so that (a/b)*b + a%b == a holds. */
    return dneg ? -(s32_)r : (s32_)r;
}

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration -- so this is
 * it, rather than an empty file that some compiler is entitled to reject. */
typedef int embcc_rt_avr_is_not_this_target;

#endif /* __AVR__ */
