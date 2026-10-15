/* mips-dsp.c -- the DSP ASE, Revision 2, in its MIPS32 encodings, as a
 * PIC32MZ's microAptiv core (`pic32mz`) has it.
 *
 * A register holds four unsigned bytes (.qb), two halfwords (.ph: Q15
 * fractions or integers) or a word (.w: Q31); the accumulators $ac0-$ac3
 * are HI and LO pairs, and DSPControl holds pos (5:0), scount (12:7),
 * the carry (13), EFI (14), the overflow flags ouflag (23:16) and the
 * compare bits ccond (27:24). An instruction that overflows or saturates
 * sets its ouflag bit, which stays set until wrdsp clears it: bit 20 for
 * an add or subtract, 21 for a multiply into a register, 22 for a shift
 * or a precision reduction, 23 for an extract, 16 + ac for an operation
 * on accumulator ac.
 *
 *   - Arithmetic: addu, subu, addq, subq (wrapping or saturating), their
 *     halving forms (adduh, addqh, subuh, subqh, rounding or not), absq_s,
 *     addsc and addwc (the carry), modsub, raddu.w.qb.
 *   - Precision: precr, precrq, precr_sra, precrq_rs, precrqu_s, and the
 *     expanding preceq, precequ and preceu.
 *   - Shifts: shll (wrapping or saturating), shrl, shra (truncating or
 *     rounding), by an immediate or a register.
 *   - Multiplies into a register: muleu_s, mulq_rs, mulq_s, muleq_s, mul
 *     and mul_s.ph.
 *   - Into an accumulator: the dot products dpa, dps, dpau, dpsu, dpaq_s,
 *     dpsq_s (straight or crossed, x), dpaqx_sa, dpsqx_sa, dpaq_sa.l.w and
 *     dpsq_sa.l.w; mulsa, mulsaq_s, maq_s, maq_sa.
 *   - Compare and pick: cmpu, cmpgu, cmpgdu, cmp, pick, packrl.
 *   - Accumulators: extr (truncating, rounding, saturating), extr_s.h,
 *     extp and extpdp (at DSPControl.pos), shilo, mthlip.
 *   - Bits: insv, bitrev, repl and replv, append, prepend, balign.
 *
 * Where the manual leaves a result open, it is QEMU's, the referee
 * tests/golden/embsim-mips.sh holds this file to (its 74Kf): extp's rt
 * is 0 when EFI is set, insv leaves rt as it was when scount is 0 or the
 * field would end past bit 32, and balign with bp 2 leaves rt as it was.
 * mips.c decodes the rest -- lwx, lhx, lbux, bposge32, rddsp, wrdsp and
 * the accumulator forms of mult, madd and msub -- and checks Status.MX
 * before calling here. */
#include "mips.h"

typedef int16_t s16;

static void flag(int bit)
{
    ms->dspctl |= 1u << bit;
}

static u32 sx8(u32 v) { return (u32)(s32)(int8_t)v; }
static u32 sx16(u32 v) { return (u32)(s32)(s16)v; }

/* ---- the lanes ------------------------------------------------------------ */

typedef u32 (*lane_fn)(u32 a, u32 b);

/* f on each byte (a quad byte), or each halfword (a pair) */
static u32 qb(u32 a, u32 b, lane_fn f)
{
    u32 r = 0;
    for (int k = 0; k < 32; k += 8)
        r |= (f((a >> k) & 0xff, (b >> k) & 0xff) & 0xff) << k;
    return r;
}

static u32 ph(u32 a, u32 b, lane_fn f)
{
    return (f(a >> 16, b >> 16) & 0xffff) << 16 | (f(a & 0xffff, b & 0xffff) & 0xffff);
}

/* the unsigned bytes and halfwords: wrapping (the overflow flagged) or
 * saturating */
static u32 addu8(u32 a, u32 b) { if (a + b > 0xff) flag(20); return a + b; }
static u32 addu8s(u32 a, u32 b) { if (a + b > 0xff) { flag(20); return 0xff; } return a + b; }
static u32 subu8(u32 a, u32 b) { if (a < b) flag(20); return a - b; }
static u32 subu8s(u32 a, u32 b) { if (a < b) { flag(20); return 0; } return a - b; }
static u32 addu16(u32 a, u32 b) { if (a + b > 0xffff) flag(20); return a + b; }
static u32 addu16s(u32 a, u32 b) { if (a + b > 0xffff) { flag(20); return 0xffff; } return a + b; }
static u32 subu16(u32 a, u32 b) { if (a < b) flag(20); return a - b; }
static u32 subu16s(u32 a, u32 b) { if (a < b) { flag(20); return 0; } return a - b; }

/* the Q15 halfwords */
static s32 clamp16(s32 v, int bit)
{
    if (v > 0x7fff) { flag(bit); return 0x7fff; }
    if (v < -0x8000) { flag(bit); return -0x8000; }
    return v;
}
static u32 addq16(u32 a, u32 b)
{
    s32 v = (s32)sx16(a) + (s32)sx16(b);
    if (v != (s16)v) flag(20);
    return (u32)v;
}
static u32 addq16s(u32 a, u32 b) { return (u32)clamp16((s32)sx16(a) + (s32)sx16(b), 20); }
static u32 subq16(u32 a, u32 b)
{
    s32 v = (s32)sx16(a) - (s32)sx16(b);
    if (v != (s16)v) flag(20);
    return (u32)v;
}
static u32 subq16s(u32 a, u32 b) { return (u32)clamp16((s32)sx16(a) - (s32)sx16(b), 20); }

/* halving: the sum or difference's bits 16:1 (or 8:1), rounded or not */
static u32 adduh(u32 a, u32 b) { return (a + b) >> 1; }
static u32 adduhr(u32 a, u32 b) { return (a + b + 1) >> 1; }
static u32 subuh(u32 a, u32 b) { return (u32)(((s32)a - (s32)b) >> 1); }
static u32 subuhr(u32 a, u32 b) { return (u32)(((s32)a - (s32)b + 1) >> 1); }
static u32 addqh(u32 a, u32 b) { return (u32)(((s32)sx16(a) + (s32)sx16(b)) >> 1); }
static u32 addqhr(u32 a, u32 b) { return (u32)(((s32)sx16(a) + (s32)sx16(b) + 1) >> 1); }
static u32 subqh(u32 a, u32 b) { return (u32)(((s32)sx16(a) - (s32)sx16(b)) >> 1); }
static u32 subqhr(u32 a, u32 b) { return (u32)(((s32)sx16(a) - (s32)sx16(b) + 1) >> 1); }

static u32 absq8(u32 a, u32 b)
{
    (void)b;
    if (a == 0x80) { flag(20); return 0x7f; }
    return (a & 0x80) ? -a : a;
}
static u32 absq16(u32 a, u32 b)
{
    (void)b;
    if (a == 0x8000) { flag(20); return 0x7fff; }
    return (a & 0x8000) ? -a : a;
}

/* the multiplies: unsigned byte by halfword, saturating; the Q15 ones
 * (-1 by -1 saturates to the largest value); the integer halfwords */
static u32 muleu(u32 a, u32 b) { if (a * b > 0xffff) { flag(21); return 0xffff; } return a * b; }
static u32 mulqrs(u32 a, u32 b)
{
    if (a == 0x8000 && b == 0x8000) { flag(21); return 0x7fff; }
    return ((u32)((s32)sx16(a) * (s32)sx16(b)) * 2 + 0x8000) >> 16;
}
static u32 mulqs(u32 a, u32 b)
{
    if (a == 0x8000 && b == 0x8000) { flag(21); return 0x7fff; }
    return ((u32)((s32)sx16(a) * (s32)sx16(b)) * 2) >> 16;
}
static u32 mul16(u32 a, u32 b)
{
    s32 v = (s32)sx16(a) * (s32)sx16(b);
    if (v != (s16)v) flag(21);
    return (u32)v;
}
static u32 mul16s(u32 a, u32 b) { return (u32)clamp16((s32)sx16(a) * (s32)sx16(b), 21); }

/* a Q15 product, as a Q31, into accumulator ac: -1 by -1 saturates */
static s32 q15mul(u32 ac, u32 a, u32 b)
{
    a &= 0xffff;
    b &= 0xffff;
    if (a == 0x8000 && b == 0x8000) {
        flag(16 + (int)ac);
        return 0x7fffffff;
    }
    return (s32)((u32)((s32)sx16(a) * (s32)sx16(b)) * 2);
}

/* the shifts, s already reduced to the element's width */
static u32 sh_s;
static u32 shll8(u32 a, u32 b)
{
    (void)b;
    if (sh_s && (a >> (8 - sh_s)))
        flag(22);
    return a << sh_s;
}
/* a left shift of a signed element of n bits: the bits shifted out and
 * the new sign must all equal the old sign, or it overflows */
static int shl_over(u32 a, u32 s, int n)
{
    s32 v = (s32)(a << (32 - n)) >> (32 - n);
    s32 w = (s32)(a << (32 - n + s)) >> (32 - n + s);
    return s && v != w;
}
static u32 shll16(u32 a, u32 b)
{
    (void)b;
    if (shl_over(a, sh_s, 16))
        flag(22);
    return a << sh_s;
}
static u32 shll16s(u32 a, u32 b)
{
    (void)b;
    if (shl_over(a, sh_s, 16)) {
        flag(22);
        return (a & 0x8000) ? 0x8000 : 0x7fff;
    }
    return a << sh_s;
}
static u32 shrl(u32 a, u32 b) { (void)b; return a >> sh_s; }
static u32 shra8(u32 a, u32 b) { (void)b; return (u32)((s32)sx8(a) >> sh_s); }
static u32 shra16(u32 a, u32 b) { (void)b; return (u32)((s32)sx16(a) >> sh_s); }
/* rounding: the bit shifted out last is added */
static u32 shra8r(u32 a, u32 b)
{
    (void)b;
    return sh_s ? (u32)((((s32)sx8(a) >> (sh_s - 1)) + 1) >> 1) : a;
}
static u32 shra16r(u32 a, u32 b)
{
    (void)b;
    return sh_s ? (u32)((((s32)sx16(a) >> (sh_s - 1)) + 1) >> 1) : a;
}
static u32 shra32r(u32 a, u32 s)
{
    return s ? (u32)((((s64)(s32)a >> (s - 1)) + 1) >> 1) : a;
}

/* ---- the accumulators ------------------------------------------------------ */

static u64 acc(u32 ac)
{
    return (u64)ms->hi[ac] << 32 | ms->lo[ac];
}

static void acc_set(u32 ac, u64 v)
{
    ms->lo[ac] = (u32)v;
    ms->hi[ac] = (u32)(v >> 32);
}

/* a 64-bit result that must fit a Q31: saturated to one, flagged */
static u64 sat_q31(u32 ac, u64 v)
{
    if ((s64)v > 0x7fffffffLL) { flag(16 + (int)ac); return 0x7fffffffu; }
    if ((s64)v < -0x80000000LL) { flag(16 + (int)ac); return (u64)(s64)-0x80000000LL; }
    return v;
}

/* extr's shift: the accumulator shifted right arithmetically, kept with
 * one more bit below (the rounding bit) and one above (the sign): bits
 * 64:0 of acc >> (s - 1), as `hi`:`lo` */
static void extr_shift(u32 ac, u32 s, u32 *hi, u64 *lo)
{
    s64 a = (s64)acc(ac);
    *hi = a < 0;
    *lo = s ? (u64)(a >> (s - 1)) : (u64)a << 1;
}

/* whether bits 64:32 of hi:lo are not one sign: the value does not fit a
 * word */
static int extr_over(u32 hi, u64 lo)
{
    u32 top = (u32)(lo >> 32);
    return !((hi & 1) == 0 && top == 0) && !((hi & 1) == 1 && top == 0xffffffffu);
}

/* extr.w, extr_r.w, extr_rs.w: ouflag 23 when the shifted value, or the
 * rounded one, does not fit a word */
static u32 extr(u32 ac, u32 s, int round, int sat)
{
    u32 hi, r;
    u64 lo;
    extr_shift(ac, s & 31, &hi, &lo);
    if (extr_over(hi, lo))
        flag(23);
    r = (u32)(lo >> 1);
    lo++;
    if (!lo)
        hi++;
    if (round)
        r = (u32)(lo >> 1);
    if (extr_over(hi, lo)) {
        flag(23);
        if (sat)
            r = (hi & 1) ? 0x80000000u : 0x7fffffffu;
    }
    return r;
}

static u32 extr_s_h(u32 ac, u32 s)
{
    s64 v = (s64)acc(ac) >> (s & 31);
    if (v > 0x7fff) { flag(23); return 0x7fff; }
    if (v < -0x8000) { flag(23); return 0xffff8000u; }
    return (u32)v;
}

/* extp and extpdp: size + 1 bits ending at pos; EFI when there are not
 * so many below it */
static u32 extp(u32 ac, u32 size, int dp)
{
    u32 pos = ms->dspctl & 0x3f;
    size &= 31;
    if ((int)pos - (int)(size + 1) < -1) {
        ms->dspctl |= 1u << 14;
        return 0;
    }
    ms->dspctl &= ~(1u << 14);
    u32 r = (u32)(acc(ac) >> (pos - size)) & (0xffffffffu >> (31 - size));
    if (dp)
        ms->dspctl = (ms->dspctl & ~0x3fu) | ((pos - (size + 1)) & 0x3f);
    return r;
}

/* shilo's shift: right when positive, left when negative */
static void shilo(u32 ac, u32 s6)
{
    s32 s = (s32)(s6 << 26) >> 26;
    u64 a = acc(ac);
    if (s > 0)
        acc_set(ac, a >> s);
    else if (s < 0)
        acc_set(ac, a << -s);
}

/* ---- the instruction ------------------------------------------------------ */

static void W(u32 n, u32 v)
{
    if (n)
        ms->r[n] = v;
}

static void ccond(u32 bits, int n)
{
    u32 m = ((1u << n) - 1) << 24;
    ms->dspctl = (ms->dspctl & ~m) | bits << 24;
}

static u32 cmp_qb(u32 a, u32 b, int how)
{
    u32 r = 0;
    for (int k = 0; k < 4; k++) {
        u32 x = (a >> (8 * k)) & 0xff, y = (b >> (8 * k)) & 0xff;
        int c = how == 0 ? x == y : how == 1 ? x < y : x <= y;
        r |= (u32)c << k;
    }
    return r;
}

static u32 cmp_ph(u32 a, u32 b, int how)
{
    u32 r = 0;
    for (int k = 0; k < 2; k++) {
        s32 x = (s16)(a >> (16 * k)), y = (s16)(b >> (16 * k));
        int c = how == 0 ? x == y : how == 1 ? x < y : x <= y;
        r |= (u32)c << k;
    }
    return r;
}

/* pick: each element from rs where its ccond bit is set, else from rt */
static u32 pick(u32 a, u32 b, int n, u32 m)
{
    u32 r = 0;
    for (int k = 0; k < n; k++) {
        u32 lane = m << (32 / n * k);
        r |= (((ms->dspctl >> (24 + k)) & 1) ? a : b) & lane;
    }
    return r;
}

static u32 trunc16(u32 a)                   /* precrq_rs: a Q31 to a Q15 */
{
    if ((s32)a > 0x7fff7fff) {
        flag(22);
        return 0x7fff;
    }
    return ((a + 0x8000) >> 16) & 0xffff;
}

static u32 reduce8(u32 a)                   /* precrqu_s: a Q15 to a byte */
{
    if (a & 0x8000) { flag(22); return 0; }
    if (a > 0x7f80) { flag(22); return 0xff; }
    return (a >> 7) & 0xff;
}

/* the pairs of bytes muleu_s and precequ/preceu take, as halfwords: the
 * left (3, 2), right (1, 0), left alternate (3, 1), right alternate (2, 0) */
static u32 bytes2(u32 a, int which)
{
    static const unsigned char at[4][2] = { { 24, 16 }, { 8, 0 }, { 24, 8 }, { 16, 0 } };
    return ((a >> at[which][0]) & 0xff) << 16 | ((a >> at[which][1]) & 0xff);
}

/* DPA.W.PH's group: into accumulator ac */
static void dpa(u32 op, u32 ac, u32 a, u32 b)
{
    u32 ah = a >> 16, al = a & 0xffff, bh = b >> 16, bl = b & 0xffff;
    u64 v = acc(ac), d;
    switch (op) {
    case 0x03: case 0x07: case 0x0b: case 0x0f: {              /* DPAU.H.QBL/QBR, DPSU */
        u32 x = bytes2(a, (op >> 2) & 1), y = bytes2(b, (op >> 2) & 1);
        d = (u64)((x >> 16) * (y >> 16) + (x & 0xff) * (y & 0xff));
        acc_set(ac, op & 8 ? v - d : v + d);
        return;
    }
    case 0x00: case 0x01: case 0x08: case 0x09: {              /* DPA, DPS, DPAX, DPSX.W.PH */
        if (op & 8) { u32 t = bh; bh = bl; bl = t; }
        d = (u64)((s64)(s32)sx16(ah) * (s32)sx16(bh) + (s64)(s32)sx16(al) * (s32)sx16(bl));
        acc_set(ac, op & 1 ? v - d : v + d);
        return;
    }
    case 0x04: case 0x05: case 0x18: case 0x19: {              /* DPAQ_S, DPSQ_S, DPAQX_S, DPSQX_S */
        if (op & 0x10) { u32 t = bh; bh = bl; bl = t; }
        d = (u64)((s64)q15mul(ac, ah, bh) + q15mul(ac, al, bl));
        acc_set(ac, op & 1 ? v - d : v + d);
        return;
    }
    case 0x1a: case 0x1b:                                       /* DPAQX_SA, DPSQX_SA.W.PH */
        d = (u64)((s64)q15mul(ac, ah, bl) + q15mul(ac, al, bh));
        acc_set(ac, sat_q31(ac, op & 1 ? v - d : v + d));
        return;
    case 0x06:                                                  /* MULSAQ_S.W.PH */
        d = (u64)((s64)q15mul(ac, ah, bh) - q15mul(ac, al, bl));
        acc_set(ac, v + d);
        return;
    case 0x02:                                                  /* MULSA.W.PH */
        d = (u64)((s64)(s32)sx16(ah) * (s32)sx16(bh) - (s64)(s32)sx16(al) * (s32)sx16(bl));
        acc_set(ac, v + d);
        return;
    case 0x0c: case 0x0d: {                                     /* DPAQ_SA.L.W, DPSQ_SA.L.W */
        u64 r;
        int over;
        if (a == 0x80000000u && b == 0x80000000u) {
            flag(16 + (int)ac);
            d = 0x7fffffffffffffffull;
        } else {
            d = (u64)((s64)(s32)a * (s32)b) << 1;
        }
        if (op == 0x0c) {
            r = v + d;
            over = (int)((~(v ^ d) & (v ^ r)) >> 63);
        } else {
            r = v - d;
            over = (int)(((v ^ d) & (v ^ r)) >> 63);
        }
        if (over) {
            flag(16 + (int)ac);
            r = (r >> 63) ? 0x7fffffffffffffffull : 0x8000000000000000ull;
        }
        acc_set(ac, r);
        return;
    }
    case 0x14: case 0x16:                                       /* MAQ_S.W.PHL, PHR */
        d = (u64)(s64)(op == 0x14 ? q15mul(ac, ah, bh) : q15mul(ac, al, bl));
        acc_set(ac, v + d);
        return;
    case 0x10: case 0x12: {                                     /* MAQ_SA.W.PHL, PHR */
        /* the sum's bits 32 and 31 must agree, or it saturates */
        u64 s = v + (u64)(s64)(op == 0x10 ? q15mul(ac, ah, bh) : q15mul(ac, al, bl));
        s32 r = (s32)s;
        if (((s >> 32) & 1) != ((s >> 31) & 1)) {
            flag(16 + (int)ac);
            r = ((s >> 32) & 1) ? (s32)0x80000000u : 0x7fffffff;
        }
        acc_set(ac, (u64)(s64)r);
        return;
    }
    }
    mips_raise(EXC_RI);
}

/* An instruction of SPECIAL3's DSP groups (function 0x0c, 0x10-0x13,
 * 0x18, 0x30, 0x31, 0x38), the DSP present and Status.MX set. */
void mips_dsp_exec(u32 i)
{
    u32 rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
    u32 op = (i >> 6) & 31, a = ms->r[rs], b = ms->r[rt], ac = rd & 3;
    switch (i & 0x3f) {
    case 0x10:                                                  /* ADDU.QB's group */
        switch (op) {
        case 0x00: W(rd, qb(a, b, addu8)); return;              /* ADDU.QB */
        case 0x01: W(rd, qb(a, b, subu8)); return;              /* SUBU.QB */
        case 0x04: W(rd, qb(a, b, addu8s)); return;             /* ADDU_S.QB */
        case 0x05: W(rd, qb(a, b, subu8s)); return;             /* SUBU_S.QB */
        case 0x06: W(rd, ph(bytes2(a, 0), b, muleu)); return;   /* MULEU_S.PH.QBL */
        case 0x07: W(rd, ph(bytes2(a, 1), b, muleu)); return;   /* MULEU_S.PH.QBR */
        case 0x08: W(rd, ph(a, b, addu16)); return;             /* ADDU.PH */
        case 0x09: W(rd, ph(a, b, subu16)); return;             /* SUBU.PH */
        case 0x0a: W(rd, ph(a, b, addq16)); return;             /* ADDQ.PH */
        case 0x0b: W(rd, ph(a, b, subq16)); return;             /* SUBQ.PH */
        case 0x0c: W(rd, ph(a, b, addu16s)); return;            /* ADDU_S.PH */
        case 0x0d: W(rd, ph(a, b, subu16s)); return;            /* SUBU_S.PH */
        case 0x0e: W(rd, ph(a, b, addq16s)); return;            /* ADDQ_S.PH */
        case 0x0f: W(rd, ph(a, b, subq16s)); return;            /* SUBQ_S.PH */
        case 0x10: {                                            /* ADDSC */
            u64 s = (u64)a + b;
            ms->dspctl = (ms->dspctl & ~(1u << 13)) | (u32)(s >> 32) << 13;
            W(rd, (u32)s);
            return;
        }
        case 0x11: {                                            /* ADDWC */
            s64 s = (s64)(s32)a + (s32)b + ((ms->dspctl >> 13) & 1);
            if (s != (s32)s)
                flag(20);
            W(rd, (u32)s);
            return;
        }
        case 0x12:                                              /* MODSUB */
            W(rd, a ? a - (b & 0xff) : (b >> 8) & 0xffff);
            return;
        case 0x14:                                              /* RADDU.W.QB */
            W(rd, (a & 0xff) + ((a >> 8) & 0xff) + ((a >> 16) & 0xff) + (a >> 24));
            return;
        case 0x16: case 0x17: {                                 /* ADDQ_S.W, SUBQ_S.W */
            s64 s = op == 0x16 ? (s64)(s32)a + (s32)b : (s64)(s32)a - (s32)b;
            if (s > 0x7fffffffLL) { flag(20); s = 0x7fffffffLL; }
            if (s < -0x80000000LL) { flag(20); s = -0x80000000LL; }
            W(rd, (u32)s);
            return;
        }
        case 0x1c: case 0x1d: {                                 /* MULEQ_S.W.PHL, PHR */
            u32 k = op == 0x1c ? 16 : 0;
            u32 x = (a >> k) & 0xffff, y = (b >> k) & 0xffff;
            if (x == 0x8000 && y == 0x8000) {
                flag(21);
                W(rd, 0x7fffffffu);
            } else {
                W(rd, (u32)((s32)sx16(x) * (s32)sx16(y)) * 2);
            }
            return;
        }
        case 0x1e: W(rd, ph(a, b, mulqs)); return;              /* MULQ_S.PH */
        case 0x1f: W(rd, ph(a, b, mulqrs)); return;             /* MULQ_RS.PH */
        }
        break;
    case 0x11:                                                  /* CMPU.EQ.QB's group */
        switch (op) {
        case 0x00: case 0x01: case 0x02:                        /* CMPU.EQ/LT/LE.QB */
            ccond(cmp_qb(a, b, (int)op), 4);
            return;
        case 0x03: W(rd, pick(a, b, 4, 0xff)); return;          /* PICK.QB */
        case 0x04: case 0x05: case 0x06:                        /* CMPGU.EQ/LT/LE.QB */
            W(rd, cmp_qb(a, b, (int)op - 4));
            return;
        case 0x08: case 0x09: case 0x0a:                        /* CMP.EQ/LT/LE.PH */
            ccond(cmp_ph(a, b, (int)op - 8), 2);
            return;
        case 0x0b: W(rd, pick(a, b, 2, 0xffff)); return;        /* PICK.PH */
        case 0x0c:                                              /* PRECRQ.QB.PH */
            W(rd, (a & 0xff000000u) | (a << 8 & 0x00ff0000u) |
                  (b >> 16 & 0xff00u) | (b >> 8 & 0xffu));
            return;
        case 0x0d:                                              /* PRECR.QB.PH */
            W(rd, (a << 8 & 0xff000000u) | (a << 16 & 0x00ff0000u) |
                  (b >> 8 & 0xff00u) | (b & 0xffu));
            return;
        case 0x0e: W(rd, a << 16 | b >> 16); return;            /* PACKRL.PH */
        case 0x0f: {                                            /* PRECRQU_S.QB.PH */
            u32 d = reduce8(a >> 16), c = reduce8(a & 0xffff);
            u32 e = reduce8(b >> 16), f = reduce8(b & 0xffff);
            W(rd, d << 24 | c << 16 | e << 8 | f);
            return;
        }
        case 0x14: W(rd, (a & 0xffff0000u) | b >> 16); return;  /* PRECRQ.PH.W */
        case 0x15: {                                            /* PRECRQ_RS.PH.W */
            u32 x = trunc16(a), y = trunc16(b);
            W(rd, x << 16 | y);
            return;
        }
        case 0x18: case 0x19: case 0x1a: {                      /* CMPGDU.EQ/LT/LE.QB */
            u32 c = cmp_qb(a, b, (int)op - 0x18);
            ccond(c, 4);
            W(rd, c);
            return;
        }
        case 0x1e: case 0x1f: {                                 /* PRECR_SRA(_R).PH.W rt, rs, sa */
            u32 x = op == 0x1e ? (u32)((s32)b >> rd) : shra32r(b, rd);
            u32 y = op == 0x1e ? (u32)((s32)a >> rd) : shra32r(a, rd);
            W(rt, (x & 0xffff) << 16 | (y & 0xffff));
            return;
        }
        }
        break;
    case 0x12:                                                  /* ABSQ_S.PH's group */
        switch (op) {
        case 0x01: W(rd, qb(b, 0, absq8)); return;              /* ABSQ_S.QB */
        case 0x02:                                              /* REPL.QB */
            W(rd, ((i >> 16) & 0xff) * 0x01010101u);
            return;
        case 0x03: W(rd, (b & 0xff) * 0x01010101u); return;     /* REPLV.QB */
        case 0x04: case 0x05: case 0x06: case 0x07:             /* PRECEQU.PH.QBL/R/LA/RA */
            W(rd, bytes2(b, (int)op - 4) << 7);
            return;
        case 0x09: W(rd, ph(b, 0, absq16)); return;             /* ABSQ_S.PH */
        case 0x0a: {                                            /* REPL.PH */
            u32 v = (u32)((s32)(i << 6) >> 22) & 0xffff;
            W(rd, v << 16 | v);
            return;
        }
        case 0x0b: W(rd, (b & 0xffff) * 0x00010001u); return;   /* REPLV.PH */
        case 0x0c: W(rd, b & 0xffff0000u); return;              /* PRECEQ.W.PHL */
        case 0x0d: W(rd, b << 16); return;                      /* PRECEQ.W.PHR */
        case 0x11:                                              /* ABSQ_S.W */
            if (b == 0x80000000u) {
                flag(20);
                W(rd, 0x7fffffffu);
            } else {
                W(rd, (s32)b < 0 ? -b : b);
            }
            return;
        case 0x1b: {                                            /* BITREV */
            u32 r = 0;
            for (int k = 0; k < 16; k++)
                r |= ((b >> k) & 1) << (15 - k);
            W(rd, r);
            return;
        }
        case 0x1c: case 0x1d: case 0x1e: case 0x1f:             /* PRECEU.PH.QBL/R/LA/RA */
            W(rd, bytes2(b, (int)op - 0x1c));
            return;
        }
        break;
    case 0x13: {                                                /* SHLL.QB's group */
        /* the shift: rs's field, or rs's register in the v forms (bit 1
         * of op) */
        u32 s = (op & 2) ? a : rs;
        switch (op) {
        case 0x00: case 0x02: sh_s = s & 7; W(rd, qb(b, 0, shll8)); return;     /* SHLL(V).QB */
        case 0x01: case 0x03: sh_s = s & 7; W(rd, qb(b, 0, shrl)); return;      /* SHRL(V).QB */
        case 0x04: case 0x06: sh_s = s & 7; W(rd, qb(b, 0, shra8)); return;     /* SHRA(V).QB */
        case 0x05: case 0x07: sh_s = s & 7; W(rd, qb(b, 0, shra8r)); return;    /* SHRA(V)_R.QB */
        case 0x08: case 0x0a: sh_s = s & 15; W(rd, ph(b, 0, shll16)); return;   /* SHLL(V).PH */
        case 0x09: case 0x0b: sh_s = s & 15; W(rd, ph(b, 0, shra16)); return;   /* SHRA(V).PH */
        case 0x0c: case 0x0e: sh_s = s & 15; W(rd, ph(b, 0, shll16s)); return;  /* SHLL(V)_S.PH */
        case 0x0d: case 0x0f: sh_s = s & 15; W(rd, ph(b, 0, shra16r)); return;  /* SHRA(V)_R.PH */
        case 0x14: case 0x16:                                   /* SHLL(V)_S.W */
            s &= 31;
            if (shl_over(b, s, 32)) {
                flag(22);
                W(rd, (b & 0x80000000u) ? 0x80000000u : 0x7fffffffu);
            } else {
                W(rd, b << s);
            }
            return;
        case 0x15: case 0x17: W(rd, shra32r(b, s & 31)); return;            /* SHRA(V)_R.W */
        case 0x19: case 0x1b: sh_s = s & 15; W(rd, ph(b, 0, shrl)); return;     /* SHRL(V).PH */
        }
        break;
    }
    case 0x18:                                                  /* ADDUH.QB's group */
        switch (op) {
        case 0x00: W(rd, qb(a, b, adduh)); return;              /* ADDUH.QB */
        case 0x01: W(rd, qb(a, b, subuh)); return;              /* SUBUH.QB */
        case 0x02: W(rd, qb(a, b, adduhr)); return;             /* ADDUH_R.QB */
        case 0x03: W(rd, qb(a, b, subuhr)); return;             /* SUBUH_R.QB */
        case 0x08: W(rd, ph(a, b, addqh)); return;              /* ADDQH.PH */
        case 0x09: W(rd, ph(a, b, subqh)); return;              /* SUBQH.PH */
        case 0x0a: W(rd, ph(a, b, addqhr)); return;             /* ADDQH_R.PH */
        case 0x0b: W(rd, ph(a, b, subqhr)); return;             /* SUBQH_R.PH */
        case 0x0c: W(rd, ph(a, b, mul16)); return;              /* MUL.PH */
        case 0x0e: W(rd, ph(a, b, mul16s)); return;             /* MUL_S.PH */
        case 0x10: case 0x11: case 0x12: case 0x13: {           /* ADDQH(_R).W, SUBQH(_R).W */
            s64 s = (op & 1) ? (s64)(s32)a - (s32)b : (s64)(s32)a + (s32)b;
            W(rd, (u32)((s + (op >> 1 & 1)) >> 1));
            return;
        }
        case 0x16: case 0x17: {                                 /* MULQ_S.W, MULQ_RS.W */
            if (a == 0x80000000u && b == 0x80000000u) {
                flag(21);
                W(rd, 0x7fffffffu);
                return;
            }
            u64 p = (u64)((s64)(s32)a * (s32)b) << 1;
            if (op == 0x17)
                p += 0x80000000u;
            W(rd, (u32)(p >> 32));
            return;
        }
        }
        break;
    case 0x30:                                                  /* DPA.W.PH's group */
        dpa(op, ac, a, b);
        return;
    case 0x0c:                                                  /* INSV */
        if (op == 0) {
            u32 pos = ms->dspctl & 31, size = (ms->dspctl >> 7) & 63;
            if (size && pos + size <= 33) {
                if (pos + size > 32)
                    size = 32 - pos;
                u32 m = (size >= 32 ? ~0u : (1u << size) - 1) << pos;
                W(rt, (b & ~m) | ((a << pos) & m));
            }
            return;
        }
        break;
    case 0x31:                                                  /* APPEND's group */
        switch (op) {
        case 0x00:                                              /* APPEND rt, rs, sa */
            if (rd)
                W(rt, b << rd | (a & ((1u << rd) - 1)));
            return;
        case 0x01:                                              /* PREPEND */
            if (rd)
                W(rt, b >> rd | a << (32 - rd));
            return;
        case 0x10: {                                            /* BALIGN */
            u32 bp = rd & 3;
            if (bp == 1 || bp == 3)
                W(rt, b << (8 * bp) | a >> (8 * (4 - bp)));
            return;
        }
        }
        break;
    case 0x38:                                                  /* EXTR.W's group */
        switch (op) {
        case 0x00: W(rt, extr(ac, rs, 0, 0)); return;           /* EXTR.W */
        case 0x01: W(rt, extr(ac, a, 0, 0)); return;            /* EXTRV.W */
        case 0x02: W(rt, extp(ac, rs, 0)); return;              /* EXTP */
        case 0x03: W(rt, extp(ac, a, 0)); return;               /* EXTPV */
        case 0x04: W(rt, extr(ac, rs, 1, 0)); return;           /* EXTR_R.W */
        case 0x05: W(rt, extr(ac, a, 1, 0)); return;            /* EXTRV_R.W */
        case 0x06: W(rt, extr(ac, rs, 1, 1)); return;           /* EXTR_RS.W */
        case 0x07: W(rt, extr(ac, a, 1, 1)); return;            /* EXTRV_RS.W */
        case 0x0a: W(rt, extp(ac, rs, 1)); return;              /* EXTPDP */
        case 0x0b: W(rt, extp(ac, a, 1)); return;               /* EXTPDPV */
        case 0x0e: W(rt, extr_s_h(ac, rs)); return;             /* EXTR_S.H */
        case 0x0f: W(rt, extr_s_h(ac, a)); return;              /* EXTRV_S.H */
        case 0x1a: shilo(ac, (i >> 20) & 63); return;           /* SHILO */
        case 0x1b: shilo(ac, a & 63); return;                   /* SHILOV */
        case 0x1f: {                                            /* MTHLIP */
            u32 pos = ms->dspctl & 0x3f;
            ms->hi[ac] = ms->lo[ac];
            ms->lo[ac] = a;
            if (pos <= 32)
                ms->dspctl = (ms->dspctl & ~0x3fu) | (pos + 32);
            return;
        }
        }
        break;
    }
    mips_raise(EXC_RI);
}
