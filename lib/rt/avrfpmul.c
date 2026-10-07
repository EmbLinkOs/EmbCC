/* IEEE-754 binary32 multiply, for AVR. Its own object: see lib/rt/avrfp.c.
 *
 * The 48-bit product is the one thing in this implementation that does not fit
 * a 32-bit word, and mul24 below is how it is carried without a 64-bit type --
 * which on this backend would be a byte-at-a-time chain through frame slots.
 */
/* RX too: GCC rx-elf's double is binary32 (-m32bit-doubles), so it needs
 * exactly this native binary32 and no binary64. */
#if defined(__AVR__) || defined(__RX__)

#include "avrfp.h"

/* The 48-bit product of two 24-bit significands, as two 32-bit words: the
 * value is hi * 2^24 + lo, with lo below 2^24.
 *
 * Split into 12-bit halves so every partial product is below 2^24 and fits a
 * 32-bit word. The four multiplies are ordinary `*`, which this backend turns
 * into calls to __mulsi3 -- four calls per float multiply, against an inline
 * 48-bit shift-and-add that measured ten times the code. lib/rt/avr.c is
 * linked by anything that multiplies at all.
 */
static void mul24(u32 a, u32 b, u32 *hi, u32 *lo)
{
    u32 al = a & 0xffful, ah = a >> 12;
    u32 bl = b & 0xffful, bh = b >> 12;
    u32 p0 = al * bl;                      /* < 2^24 */
    u32 p1 = al * bh + ah * bl;            /* < 2^25 */
    u32 p2 = ah * bh;                      /* < 2^24 */
    u32 t  = p0 + ((p1 & 0xffful) << 12);  /* < 2^25 */

    *lo = t & 0xfffffful;
    *hi = p2 + (p1 >> 12) + (t >> 24);
}

float __mulsf3(float a, float b)
{
    u32 ua = f2u(a), ub = f2u(b);
    int s = fsign(ua) ^ fsign(ub);
    u32 ma, mb;
    int ea, eb;

    if (is_nan(ua) || is_nan(ub))
        return u2f(QNAN);
    if (is_inf(ua) || is_inf(ub)) {
        u32 other = is_inf(ua) ? ub : ua;
        if (fexp(other) == 0 && fmant(other) == 0)
            return u2f(QNAN);                  /* inf * 0 */
        return u2f(pack(s, EXPMAX, 0));
    }
    __avrfp_unpack(ua, &ma, &ea);
    __avrfp_unpack(ub, &mb, &eb);
    if (ma == 0 || mb == 0)
        return u2f(pack(s, 0, 0));
    /* Normalise both significands to exactly 24 bits first. __avrfp_unpack() leaves a
     * subnormal's significand where it lay, which is fine for add and compare
     * -- they work from the exponents -- but here it decides WHERE the product
     * lands, and the first version of this got it wrong: it folded the low
     * word into sticky on the assumption the high word held bits 47..24, and
     * for 7.0f * 1.4e-45f the entire product was in the low word, so the
     * answer came out zero. With both operands normalised the product is
     * always 47 or 48 bits and the high word is always bits 47..24, so the
     * shift down to 27 is a constant 21 places with no loop at all.
     */
    while (ma < 0x800000ul) { ma += ma; ea--; }
    while (mb < 0x800000ul) { mb += mb; eb--; }
    {
        u32 hi, lo;

        mul24(ma, mb, &hi, &lo);
        /* hi * 2^24 + lo, shifted right 21: the low three bits of the result
         * are hi's, the rest come off the top of lo, and the 21 bits below
         * that are the sticky. Nothing is guessed at and nothing is lost. */
        return __avrfp_round(s, (hi << 3) | (lo >> 21),
                          ea + eb + 21, (lo & 0x1ffffful) != 0);
    }
}

#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avrfpmul_is_not_this_target;

#endif /* __AVR__ || __RX__ */
