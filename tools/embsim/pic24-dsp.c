/* pic24-dsp.c -- a dsPIC33E's DSP engine and the modulo and bit-reversed
 * addressing of its address generators, for the PIC24 core (pic24.c),
 * by Microchip's "16-Bit MCU and DSC Programmer's Reference Manual"
 * (DS70000157) and the CPU and data memory sections of the dsPIC33F
 * family reference manual (DS70204, DS70202).
 *
 *   - The accumulators A and B: 40 bits, at 0x22-0x2C as ACCxL, ACCxH and
 *     ACCxU (which reads sign-extended to 16 bits). Written there, they
 *     change no status bit.
 *   - The MAC class: MAC, MPY, MPY.N, MSC of two of W4-W7 or of one
 *     squared, ED and EDAC, CLR and MOVSAC; with their prefetches, a word
 *     from X space through W8 or W9 ([Wx], [Wx]+=k, [Wx]-=k, [W9+W12])
 *     and one from Y space through W10 or W11 into W4-W7 (an address of
 *     the other space reads 0), and the other accumulator's write-back
 *     to W13 or [W13]+=2, rounded. The multiplier's operands signed,
 *     unsigned or, CORCON.US 10, W4 and W6 unsigned and W5 and W7 signed;
 *     the product shifted left once unless CORCON.IF (integer).
 *   - ADD and SUB of the accumulators, NEG; ADD Ws and LAC with their
 *     shift (-8 to 7, right when positive) of the word placed in bits
 *     31:16; SAC and SAC.R with theirs; SFTAC by a literal or by Wb (past
 *     16 bits either way, the math error trap and no change); the MCU
 *     multiplies into an accumulator (MUL.SS, .SU, .US, .UU Wb, Ws, Acc).
 *   - The adder's results through the saturation logic, the target's
 *     SATA or SATB clear: 40 bits, a carry out of bit 39 (the sign lost)
 *     setting SA or SB; normal (ACCSAT clear): 1.31, saturated to
 *     0x007FFFFFFF or 0xFF80000000, SA or SB set; super: 9.31, to
 *     0x7FFFFFFFFF or 0x8000000000. OA and OB say the result does not fit
 *     1.31 (never, normal); SA and SB stay set until the program clears
 *     them. INTCON1's OVATE, OVBTE and COVTE make the math error trap of
 *     an overflow and of a catastrophic one (pic24-intc.c).
 *   - SAC.R and the write-back round as CORCON.RND says: conventionally
 *     (up from 0x8000) or convergently (0x8000 up when bit 16 is set);
 *     with CORCON.SATDW, SAC and SAC.R saturate to 0x7FFF or 0x8000
 *     what bits 39:16 do not hold; the write-back never.
 *   - The address generators: the X AGU's modulo buffer (MODCON.XMODEN,
 *     its register XWM, XMODSRT to XMODEND) for every instruction's
 *     indirect address, the Y AGU's (YMODEN, YWM, YMODSRT to YMODEND) for
 *     the prefetches through W10 and W11: a register stepped past the
 *     end comes back the buffer's length (stepping up; past the start
 *     stepping down), an offset address likewise; bit-reversed
 *     addressing (XBREV.BREN, MODCON.BWM) for the X AGU's word writes
 *     through [Wn++] and [++Wn]: the register's bits under the modifier
 *     XB counted in reverse (a carry to the right). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define F(hi, lo) (i >> (lo) & ((1u << ((hi) - (lo) + 1)) - 1))

#define MAX40 (((s64)1 << 39) - 1)
#define MIN40 (-((s64)1 << 39))
#define MAX32 (((s64)1 << 31) - 1)
#define MIN32 (-((s64)1 << 31))

/* v's low 40 bits, sign-extended */
static s64 wrap40(s64 v)
{
    u64 x = (u64)v & 0xffffffffffull;
    return (s64)(x & 0x8000000000ull ? x | 0xffffff0000000000ull : x);
}

static void sr_summary(void)
{
    ps->sr = (u16)(ps->sr & ~(SR_OAB | SR_SAB));
    if (ps->sr & (SR_OA | SR_OB))
        ps->sr |= SR_OAB;
    if (ps->sr & (SR_SA | SR_SB))
        ps->sr |= SR_SAB;
}

/* ---- the accumulators' registers ------------------------------------------------- */

u16 pic24_acc_read(u32 a)
{
    s64 v = ps->acc[a >= 0x28];
    switch ((a - 0x22) % 6) {
    case 0: return (u16)v;
    case 2: return (u16)(v >> 16);
    default: return (u16)(int16_t)(int8_t)(v >> 32);    /* ACCxU, sign-extended */
    }
}

void pic24_acc_write(u32 a, u16 v)
{
    int n = a >= 0x28;
    u64 x = (u64)ps->acc[n];
    switch ((a - 0x22) % 6) {
    case 0: x = (x & ~0xffffull) | v; break;
    case 2: x = (x & ~0xffff0000ull) | (u64)v << 16; break;
    default: x = (x & 0xffffffffull) | (u64)(v & 0xff) << 32; break;
    }
    ps->acc[n] = wrap40((s64)x);
}

/* ---- the adder and the saturation logic ------------------------------------------ */

/* a result of the adder into accumulator a: saturated as CORCON says,
 * OA or OB, SA or SB, and the math error trap INTCON1 makes of them */
static void acc_result(int a, s64 v)
{
    u16 cc = ps->corcon, o = a ? SR_OB : SR_OA, sat_bit = a ? SR_SB : SR_SA;
    int sat = (cc & (a ? CORCON_SATB : CORCON_SATA)) != 0, lost;
    s64 r;
    if (!sat) {
        r = wrap40(v);
        lost = v > MAX40 || v < MIN40;          /* the sign destroyed */
    } else if (!(cc & CORCON_ACCSAT)) {
        r = v > MAX32 ? MAX32 : v < MIN32 ? MIN32 : v;
        lost = r != v;
    } else {
        r = v > MAX40 ? MAX40 : v < MIN40 ? MIN40 : v;
        lost = r != v;
    }
    ps->acc[a] = r;
    int over = r > MAX32 || r < MIN32;          /* into the guard bits */
    ps->sr = (u16)((ps->sr & ~o) | (over ? o : 0) | (lost ? sat_bit : 0));
    sr_summary();
    if (over)
        pic24_core_math(a ? PIC24_MATH_OVB : PIC24_MATH_OVA);
    if (lost && !sat)
        pic24_core_math(a ? PIC24_MATH_COVB : PIC24_MATH_COVA);
}

void pic24_dsp_mul_acc(int a, s64 product)
{
    ps->acc[a] = wrap40(product);
    ps->changed = 1;
}

/* the multiplier: W m times W n, signed, unsigned or mixed as CORCON.US
 * says, 1.31 (shifted left once) unless CORCON.IF */
static s64 product(int m, int n)
{
    int us = ps->corcon >> 12 & 3;
    int sm = us == 1 ? 0 : us == 2 ? (m == 5 || m == 7) : 1;
    int sn = us == 1 ? 0 : us == 2 ? (n == 5 || n == 7) : 1;
    s64 x = sm ? (s64)(int16_t)ps->w[m] : (s64)ps->w[m];
    s64 y = sn ? (s64)(int16_t)ps->w[n] : (s64)ps->w[n];
    s64 p = x * y;
    return (ps->corcon & CORCON_IF) ? p : 2 * p;
}

/* bits 31:16 of a 40-bit value rounded as CORCON.RND says: the 40-bit
 * result */
static s64 round40(s64 v)
{
    u32 lsw = (u32)v & 0xffff;
    int up = (ps->corcon & CORCON_RND) ? lsw >= 0x8000 :
             lsw > 0x8000 || (lsw == 0x8000 && (v >> 16 & 1));
    return wrap40(v + (up ? 0x10000 : 0));
}

/* a 40-bit value shifted by k (-16 to 16; right when positive) through
 * the 40-bit barrel shifter */
static s64 shifted40(s64 v, int k)
{
    return k >= 0 ? v >> k : wrap40((s64)((u64)v << -k));
}

/* ---- the address generators -------------------------------------------------------- */

static int xmod(int r)
{
    return (ps->modcon & 0x8000) && (ps->modcon & 15) == r && r != 15;
}

static int ymod(int r)
{
    return (ps->modcon & 0x4000) && (ps->modcon >> 4 & 15) == r && r != 15;
}

static u16 modulo(u16 a, int up, u16 lo, u16 hi)
{
    u16 len = (u16)(hi - lo + 1);
    if (up && a > hi)
        return (u16)(a - len);
    if (!up && a < lo)
        return (u16)(a + len);
    return a;
}

/* the reverse-carry addition of the modifier XB (as a word address) */
static u16 bitrev(u16 w)
{
    u16 bit = (u16)((ps->xbrev & 0x7fff) << 1);
    while (bit >= 2 && (w & bit)) {
        w = (u16)(w & ~bit);
        bit >>= 1;
    }
    if (bit >= 2)
        w |= bit;
    return w;
}

u16 pic24_agu_step(int r, u16 w, int d, int write, int y)
{
    int bwm = ps->modcon >> 8 & 15;
    if (write && d == 2 && !y && (ps->xbrev & 0x8000) && bwm == r && r != 15 &&
        (ps->xbrev & 0x7fff))
        return bitrev(w);
    u16 a = (u16)(w + d);
    if (y ? ymod(r) : xmod(r))
        return y ? modulo(a, d > 0, ps->ymodsrt, ps->ymodend)
                 : modulo(a, d > 0, ps->xmodsrt, ps->xmodend);
    return a;
}

u16 pic24_agu_offset(int r, u16 ea, int neg, int y)
{
    if (y ? ymod(r) : xmod(r))
        return y ? modulo(ea, !neg, ps->ymodsrt, ps->ymodend)
                 : modulo(ea, !neg, ps->xmodsrt, ps->xmodend);
    return ea;
}

/* ---- the MAC class ------------------------------------------------------------------ */

/* a prefetch, X's (iiii, y 0) or Y's (jjjj): 0 when the field says none,
 * else 1 with the word in *v and its register stepped; -1 with the run
 * ended or a trap raised. An address of the other space reads 0. */
static int prefetch(int f, int y, u16 *v)
{
    if (f == 4)
        return 0;
    int r = (y ? 10 : 8) + (f >> 3);
    if (!pic24_core_pointer(r))
        return -1;
    u16 w = ps->w[r];
    u32 ea = w;
    if (f == 12) {                              /* [W9+W12], [W11+W12] */
        if (!pic24_core_pointer(12))
            return -1;
        ea = pic24_agu_offset(r, (u16)(w + ps->w[12]), (ps->w[12] & 0x8000) != 0, y);
    } else {
        int k = f & 7, d = k < 4 ? 2 * k : 2 * k - 16;
        if (d)
            pic24_core_set_w(r, pic24_agu_step(r, w, d, 0, y));
    }
    int in_y = ea >= ps->y_lo && ea < ps->y_hi;
    if (y != in_y) {
        *v = 0;
        return 1;
    }
    return pic24_core_read(ea, 2, v) ? -1 : 1;
}

/* the other accumulator's write-back (aa: 00 W13, 01 [W13]+=2, 10 none) */
static int write_back(int a, int aa)
{
    if (aa == 2)
        return 0;
    u16 v = (u16)(round40(ps->acc[!a]) >> 16);
    return pic24_core_dst(aa ? 3 : 0, 13, -1, 2, v);
}

/* the MAC class's common part: the prefetches into *x and *y (their
 * registers Wxd, Wyd written after the operation, by finish) */
struct pre {
    int nx, ny, xd, yd;
    u16 x, y;
};

static int fetch(u32 i, struct pre *p)
{
    p->xd = 4 + (int)F(13, 12);
    p->yd = 4 + (int)F(11, 10);
    p->x = p->y = 0;
    p->nx = prefetch((int)F(9, 6), 0, &p->x);
    if (p->nx < 0)
        return -1;
    p->ny = prefetch((int)F(5, 2), 1, &p->y);
    return p->ny < 0 ? -1 : 0;
}

static void finish(const struct pre *p)
{
    if (p->nx > 0)
        pic24_core_set_w(p->xd, p->x);
    if (p->ny > 0)
        pic24_core_set_w(p->yd, p->y);
}

/* MAC, MPY, MPY.N, MSC Wm*Wn; CLR; MOVSAC: 1100 0mmm AXxx yyii iijj jjaa */
static int mac_class(u32 i)
{
    static const signed char wm[8] = { 4, 4, 4, -1, 5, 5, 6, -1 };
    static const signed char wn[8] = { 5, 6, 7, -1, 6, 7, 7, -1 };
    int mmm = (int)F(18, 16), a = (int)F(15, 15), neg = (int)F(14, 14);
    int aa = (int)F(1, 0);
    struct pre p;
    if ((mmm == 3 || mmm == 7) && (neg || aa == 3))
        return pic24_core_bad_opcode();
    s64 prod = wm[mmm] >= 0 ? product(wm[mmm], wn[mmm]) : 0;
    if (fetch(i, &p))
        return 1;
    if (mmm == 3) {                             /* CLR: OA/SA or OB/SB too */
        if (write_back(a, aa))
            return 1;
        ps->acc[a] = 0;
        ps->sr = (u16)(ps->sr & ~(a ? SR_OB | SR_SB : SR_OA | SR_SA));
        sr_summary();
    } else if (mmm == 7) {                      /* MOVSAC */
        if (write_back(a, aa))
            return 1;
    } else if (aa == 3) {                       /* MPY, MPY.N */
        acc_result(a, neg ? -prod : prod);
    } else {                                    /* MAC, MSC */
        if (write_back(a, aa))
            return 1;
        acc_result(a, neg ? ps->acc[a] - prod : ps->acc[a] + prod);
    }
    finish(&p);
    ps->changed = 1;
    return 1;
}

/* MAC, MPY Wm*Wm; ED, EDAC: 1111 00mm AXxx yyii iijj jjoo */
static int square_class(u32 i)
{
    int m = 4 + (int)F(17, 16), a = (int)F(15, 15), ed = (int)F(14, 14);
    int op = (int)F(1, 0);
    struct pre p;
    if (ed != (op >= 2) || (ed && F(11, 10)))
        return pic24_core_bad_opcode();
    s64 prod = product(m, m);
    if (fetch(i, &p))
        return 1;
    switch (op) {
    case 0: acc_result(a, ps->acc[a] + prod); break;    /* MAC Wm*Wm */
    case 1: acc_result(a, prod); break;                 /* MPY Wm*Wm */
    case 2: acc_result(a, ps->acc[a] + prod); break;    /* EDAC */
    default: acc_result(a, prod); break;                /* ED */
    }
    if (ed) {                                   /* Wxd: [Wx] - [Wy] */
        p.x = (u16)(p.x - p.y);
        p.nx = 1;
        p.ny = 0;
    }
    finish(&p);
    ps->changed = 1;
    return 1;
}

/* ---- the accumulator instructions -------------------------------------------------- */

/* a word into bits 31:16, sign-extended, shifted by Slit4 (rrrr) */
static s64 word_in(u16 w, int r)
{
    s64 v = (s64)(int16_t)w * 65536;
    int k = (r ^ 8) - 8;
    return k >= 0 ? v >> k : v * ((s64)1 << -k);
}

/* SAC, SAC.R Acc, #Slit4, Wd: 1100 110R Awww wrrr rhhh dddd */
static int store(u32 i, int rounded)
{
    int a = (int)F(15, 15), k = (int)(F(10, 7) ^ 8) - 8;
    s64 v = shifted40(ps->acc[a], k);
    if (rounded)
        v = round40(v);
    s64 hi = v >> 16;                           /* bits 39:16 */
    u16 w = (u16)hi;
    if (ps->corcon & CORCON_SATDW)
        w = hi > 0x7fff ? 0x7fff : hi < -0x8000 ? 0x8000 : w;
    pic24_core_dst((int)F(6, 4), (int)F(3, 0), (int)F(14, 11), 2, w);
    return 1;
}

int pic24_dsp_exec(u32 i)
{
    u32 op8 = i >> 16 & 0xff;
    int a = (int)F(15, 15);
    u16 w;
    if (op8 >= 0xf0)
        return square_class(i);
    if (op8 <= 0xc7)
        return mac_class(i);
    switch (op8) {
    case 0xc8: {                                /* SFTAC Acc, #Slit6 or Wb */
        int k;
        if (i & 0x7f80 || (!F(6, 6) && F(5, 4)))
            return pic24_core_bad_opcode();
        k = F(6, 6) ? (int)F(5, 0) : ps->w[i & 15] & 0x3f;
        k = (k ^ 0x20) - 0x20;
        if (k > 16 || k < -16) {
            pic24_core_math(PIC24_MATH_SFTAC);
            return 1;
        }
        acc_result(a, k >= 0 ? ps->acc[a] >> k : ps->acc[a] * ((s64)1 << -k));
        ps->changed = 1;
        return 1;
    }
    case 0xc9:                                  /* ADD Ws, #Slit4, Acc */
    case 0xca:                                  /* LAC Ws, #Slit4, Acc */
        if (pic24_core_src((int)F(6, 4), (int)F(3, 0), (int)F(14, 11), 2, &w))
            return 1;
        acc_result(a, (op8 == 0xc9 ? ps->acc[a] : 0) + word_in(w, (int)F(10, 7)));
        ps->changed = 1;
        return 1;
    case 0xcb:                                  /* ADD, NEG, SUB Acc */
        if (i & 0x0fff)
            return pic24_core_bad_opcode();
        switch (F(14, 12)) {
        case 0: acc_result(a, ps->acc[0] + ps->acc[1]); break;
        case 1: acc_result(a, -ps->acc[a]); break;
        case 3: acc_result(a, ps->acc[a] - ps->acc[!a]); break;
        default: return pic24_core_bad_opcode();
        }
        ps->changed = 1;
        return 1;
    case 0xcc:                                  /* SAC */
        return store(i, 0);
    case 0xcd:                                  /* SAC.R */
        return store(i, 1);
    }
    return pic24_core_bad_opcode();             /* 0xCE: the dsPIC33C's */
}
