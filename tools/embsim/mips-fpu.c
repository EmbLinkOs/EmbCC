/* mips-fpu.c -- a MIPS32r2 FPU (coprocessor 1), as a PIC32MZ EF has one:
 * 64-bit registers (Status.FR 1) or 32 of 32 bits paired for a double
 * (FR 0), single and double precision, the word and long-word
 * conversions, in IEEE 754-2008 mode (FCSR.NAN2008 and ABS2008 set).
 *
 *   - Loads and stores: lwc1, ldc1, swc1, sdc1, and COP1X's indexed
 *     lwxc1, ldxc1, luxc1, swxc1, sdxc1, suxc1.
 *   - Moves: mfc1, mtc1, mfhc1, mthc1, cfc1 and ctc1 (FIR, FCCR, FEXR,
 *     FENR, FCSR), mov, movf and movt (on an FCC bit, and the integer
 *     movf/movt), movz and movn.
 *   - Arithmetic: add, sub, mul, div, sqrt, recip, rsqrt, abs and neg
 *     (the 2008 kind: the sign alone, never a NaN check), and COP1X's
 *     madd, msub, nmadd and nmsub, unfused as Release 2 has them (the
 *     product rounded, then the sum).
 *   - Conversions: cvt.s, cvt.d, cvt.w, cvt.l (FCSR.RM), round, trunc,
 *     ceil and floor to a word or a long word -- a NaN gives 0 and an
 *     overflow the largest integer of its sign, as the 2008 mode has it.
 *   - Compares: c.cond.fmt, the sixteen conditions into FCC0-FCC7, and
 *     bc1f, bc1t, bc1fl and bc1tl.
 * Each arithmetic instruction sets FCSR's Cause and ors its Flags; a
 * cause whose Enable is set is a floating-point exception (Cause.ExcCode
 * 15), and the destination is not written. The arithmetic is ieee754.c's:
 * a NaN an operation makes is the default one. Not modelled: paired
 * singles (a Reserved Instruction: FIR.PS is 0), flush to zero (FCSR.FS
 * is kept but subnormals are computed), the Unimplemented Operation
 * exception (E). */
#include "ieee754.h"
#include "mips.h"

#define FCSR_CAUSE_SH 12
#define FCSR_ENABLE_SH 7
#define FCSR_FLAG_SH 2
#define FCSR_FS (1u << 24)
#define FCSR_2008 (3u << 18)        /* NAN2008, ABS2008: read-only ones */

void mips_fp_reset(struct mips_state *k)
{
    for (int i = 0; i < 32; i++)
        k->fpr[i] = 0;
    k->fcsr = k->fpu ? FCSR_2008 : 0;
    /* F64, L, W, D, S and Has2008; the M5150's processor ID */
    k->fir = k->fpu ? 0x00f30000u | 0xa7u << 8 : 0;
}

static int fr(void)
{
    return (ms->status >> 26) & 1;
}

static u64 rd_s(u32 r)
{
    return (u32)ms->fpr[r];
}

static void wr_s(u32 r, u64 v)
{
    ms->fpr[r] = (ms->fpr[r] & ~0xffffffffull) | (u32)v;
}

static u64 rd_d(u32 r)
{
    if (fr())
        return ms->fpr[r];
    return (u32)ms->fpr[r & ~1u] | (u64)(u32)ms->fpr[r | 1] << 32;
}

static void wr_d(u32 r, u64 v)
{
    if (fr()) {
        ms->fpr[r] = v;
        return;
    }
    wr_s(r & ~1u, (u32)v);
    wr_s(r | 1, v >> 32);
}

static u64 rd(int d, u32 r)
{
    return d ? rd_d(r) : rd_s(r);
}

static void wr(int d, u32 r, u64 v)
{
    if (d)
        wr_d(r, v);
    else
        wr_s(r, v);
}

/* FCSR.RM: RN, RZ, RP, RM */
static int rmode(void)
{
    static const int map[4] = { IEEE_RNE, IEEE_RTZ, IEEE_RUP, IEEE_RDN };
    return map[ms->fcsr & 3];
}

/* an arithmetic instruction's exceptions: Cause set, Flags ored, and an
 * enabled one taken (0: the result is not written) */
static int done(u32 fl)
{
    ms->fcsr = (ms->fcsr & ~(0x3fu << FCSR_CAUSE_SH)) | fl << FCSR_CAUSE_SH;
    if (fl & (ms->fcsr >> FCSR_ENABLE_SH) & 0x1f) {
        mips_raise(EXC_FPE);
        return 0;
    }
    ms->fcsr |= fl << FCSR_FLAG_SH;
    return 1;
}

static int fcc(u32 cc)
{
    return (int)((ms->fcsr >> (cc ? 24 + cc : 23)) & 1);
}

static void set_fcc(u32 cc, int v)
{
    u32 bit = 1u << (cc ? 24 + cc : 23);
    ms->fcsr = v ? ms->fcsr | bit : ms->fcsr & ~bit;
}

/* to a word or a long word, by rm: a NaN 0, an overflow the largest of
 * its sign (2008) */
static u64 to_int(int d, u64 a, int bits, int rm, u32 *fl)
{
    if (ieee_isnan(d, a)) {
        *fl |= IEEE_NV;
        return 0;
    }
    u64 v = ieee_to_int(d, a, bits, 0, rm, fl);
    return bits == 32 ? (u32)v : v;
}

/* c.cond.fmt: the condition's less, equal and unordered bits, and its
 * signalling bit (an unordered compare is then invalid) */
static int compare(int d, u64 a, u64 b, u32 cond, u32 *fl)
{
    int un = ieee_isnan(d, a) || ieee_isnan(d, b);
    int lt = 0, eq = 0;
    if (un) {
        if ((cond & 8) || ieee_issnan(d, a) || ieee_issnan(d, b))
            *fl |= IEEE_NV;
    } else {
        u32 q = 0;
        lt = ieee_cmp(d, a, b, 1, &q);
        eq = ieee_cmp(d, a, b, 2, &q);
    }
    return ((cond & 4) && lt) || ((cond & 2) && eq) || ((cond & 1) && un);
}

static void arith(int d, u32 fn, u32 ft, u32 fs, u32 fd, u32 i)
{
    u64 a = rd(d, fs), b = rd(d, ft), r = 0;
    u64 sign = d ? 1ull << 63 : 1ull << 31;
    u32 fl = 0;
    int rm = rmode();
    switch (fn) {
    case 0x00: r = ieee_add(d, a, b, 0, rm, &fl); break;           /* add */
    case 0x01: r = ieee_add(d, a, b, 1, rm, &fl); break;           /* sub */
    case 0x02: r = ieee_mul(d, a, b, rm, &fl); break;              /* mul */
    case 0x03: r = ieee_div(d, a, b, rm, &fl); break;              /* div */
    case 0x04: r = ieee_sqrt(d, a, rm, &fl); break;                /* sqrt */
    case 0x05: wr(d, fd, a & ~sign); return;                       /* abs */
    case 0x06: wr(d, fd, a); return;                               /* mov */
    case 0x07: wr(d, fd, a ^ sign); return;                        /* neg */
    case 0x08: case 0x09: case 0x0a: case 0x0b:                    /* round.l ... floor.l */
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: {                  /* round.w ... floor.w */
        static const int m[4] = { IEEE_RNE, IEEE_RTZ, IEEE_RUP, IEEE_RDN };
        r = to_int(d, a, fn >= 0x0c ? 32 : 64, m[fn & 3], &fl);
        if (done(fl)) {
            if (fn >= 0x0c)
                wr_s(fd, r);
            else
                wr_d(fd, r);
        }
        return;
    }
    case 0x11:                                                     /* movf.fmt, movt.fmt */
        if (fcc((i >> 18) & 7) == (int)((i >> 16) & 1))
            wr(d, fd, a);
        return;
    case 0x12: if (!ms->r[ft]) wr(d, fd, a); return;               /* movz.fmt */
    case 0x13: if (ms->r[ft]) wr(d, fd, a); return;                /* movn.fmt */
    case 0x15:                                                     /* recip */
        r = ieee_div(d, d ? 0x3ff0000000000000ull : 0x3f800000u, a, rm, &fl);
        break;
    case 0x16: {                                                   /* rsqrt */
        u64 q = ieee_sqrt(d, a, rm, &fl);
        r = ieee_div(d, d ? 0x3ff0000000000000ull : 0x3f800000u, q, rm, &fl);
        break;
    }
    case 0x20:                                                     /* cvt.s.d */
        if (!d) {
            mips_raise(EXC_RI);
            return;
        }
        r = ieee_convert(0, a, rm, &fl);
        if (done(fl))
            wr_s(fd, r);
        return;
    case 0x21:                                                     /* cvt.d.s */
        if (d) {
            mips_raise(EXC_RI);
            return;
        }
        r = ieee_convert(1, a, rm, &fl);
        if (done(fl))
            wr_d(fd, r);
        return;
    case 0x24: case 0x25:                                          /* cvt.w, cvt.l */
        r = to_int(d, a, fn == 0x24 ? 32 : 64, rm, &fl);
        if (done(fl)) {
            if (fn == 0x24)
                wr_s(fd, r);
            else
                wr_d(fd, r);
        }
        return;
    default:
        if (fn >= 0x30) {                                          /* c.cond.fmt */
            int c = compare(d, a, b, fn & 15, &fl);
            if (done(fl))
                set_fcc((i >> 8) & 7, c);
            return;
        }
        mips_raise(EXC_RI);
        return;
    }
    if (done(fl))
        wr(d, fd, r);
}

/* cvt.s.w, cvt.d.w, cvt.s.l, cvt.d.l */
static void from_int(int l, u32 fn, u32 fs, u32 fd)
{
    u64 v = l ? rd_d(fs) : rd_s(fs);
    u32 fl = 0;
    if (fn != 0x20 && fn != 0x21) {
        mips_raise(EXC_RI);
        return;
    }
    int d = fn == 0x21;
    u64 r = ieee_from_int(d, v, l ? 64 : 32, 0, rmode(), &fl);
    if (done(fl))
        wr(d, fd, r);
}

/* COP1X: the indexed loads and stores, and the multiply-adds */
static void cop1x(u32 i)
{
    u32 base = (i >> 21) & 31, idx = (i >> 16) & 31, fs = (i >> 11) & 31;
    u32 fd = (i >> 6) & 31, fn = i & 0x3f;
    u32 va = ms->r[base] + ms->r[idx];
    switch (fn) {
    case 0x00: {                                                   /* lwxc1 */
        u32 v = mips_load(va, 4);
        if (!ms->exc)
            wr_s(fd, v);
        return;
    }
    case 0x01: case 0x05: {                                        /* ldxc1, luxc1 */
        if (fn == 0x05)
            va &= ~7u;
        if (va & 7) {
            mips_raise(EXC_ADEL);
            ms->badvaddr = va;
            return;
        }
        u32 lo = mips_load(va, 4), hi = mips_load(va + 4, 4);
        if (!ms->exc)
            wr_d(fd, lo | (u64)hi << 32);
        return;
    }
    case 0x08: mips_store(va, 4, (u32)rd_s(fs)); return;           /* swxc1 */
    case 0x09: case 0x0d: {                                        /* sdxc1, suxc1 */
        if (fn == 0x0d)
            va &= ~7u;
        if (va & 7) {
            mips_raise(EXC_ADES);
            ms->badvaddr = va;
            return;
        }
        u64 v = rd_d(fs);
        mips_store(va, 4, (u32)v);
        mips_store(va + 4, 4, (u32)(v >> 32));
        return;
    }
    case 0x0f: return;                                             /* prefx */
    }
    u32 op = fn >> 3, fmt = fn & 7, fr_ = base;
    if (op < 4 || fmt > 1) {
        mips_raise(EXC_RI);
        return;
    }
    int d = (int)fmt, rm = rmode();
    u32 fl = 0;
    u64 p = ieee_mul(d, rd(d, fs), rd(d, idx), rm, &fl);
    u64 r = ieee_add(d, p, rd(d, fr_), op & 1, rm, &fl);   /* madd, msub */
    if (op >= 6 && !ieee_isnan(d, r))                      /* nmadd, nmsub */
        r ^= d ? 1ull << 63 : 1ull << 31;
    if (done(fl))
        wr(d, fd, r);
}

void mips_fp_exec(u32 i)
{
    u32 op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31;
    u32 fs = (i >> 11) & 31, fd = (i >> 6) & 31;
    s32 imm = (s32)(int16_t)(i & 0xffff);
    u32 va = ms->r[rs] + (u32)imm;
    switch (op) {
    case 0x00:                                                     /* MOVF, MOVT */
        if (fcc((i >> 18) & 7) == (int)((i >> 16) & 1) && fs)
            ms->r[fs] = ms->r[rs];
        return;
    case 0x31: {                                                   /* lwc1 */
        u32 v = mips_load(va, 4);
        if (!ms->exc)
            wr_s(rt, v);
        return;
    }
    case 0x35: {                                                   /* ldc1 */
        if (va & 7) {
            mips_raise(EXC_ADEL);
            ms->badvaddr = va;
            return;
        }
        u32 lo = mips_load(va, 4), hi = mips_load(va + 4, 4);
        if (!ms->exc)
            wr_d(rt, lo | (u64)hi << 32);
        return;
    }
    case 0x39: mips_store(va, 4, (u32)rd_s(rt)); return;           /* swc1 */
    case 0x3d: {                                                   /* sdc1 */
        if (va & 7) {
            mips_raise(EXC_ADES);
            ms->badvaddr = va;
            return;
        }
        u64 v = rd_d(rt);
        mips_store(va, 4, (u32)v);
        mips_store(va + 4, 4, (u32)(v >> 32));
        return;
    }
    case 0x13: cop1x(i); return;
    }
    /* COP1 */
    switch (rs) {
    case 0x00: if (rt) ms->r[rt] = (u32)ms->fpr[fs]; return;      /* mfc1 */
    case 0x03:                                                     /* mfhc1 */
        if (rt)
            ms->r[rt] = fr() ? (u32)(ms->fpr[fs] >> 32) : (u32)ms->fpr[fs | 1];
        return;
    case 0x04: wr_s(fs, ms->r[rt]); return;                        /* mtc1 */
    case 0x07:                                                     /* mthc1 */
        if (fr())
            ms->fpr[fs] = (ms->fpr[fs] & 0xffffffffull) | (u64)ms->r[rt] << 32;
        else
            wr_s(fs | 1, ms->r[rt]);
        return;
    case 0x02: {                                                   /* cfc1 */
        u32 v;
        switch (fs) {
        case 0: v = ms->fir; break;
        case 25: v = (ms->fcsr >> 24 & 0xfe) | (ms->fcsr >> 23 & 1); break;
        case 26: v = ms->fcsr & 0x0003f07cu; break;
        case 28: v = (ms->fcsr & 0xf83u) | (ms->fcsr & FCSR_FS ? 4u : 0u); break;
        case 31: v = ms->fcsr; break;
        default: mips_raise(EXC_RI); return;
        }
        if (rt)
            ms->r[rt] = v;
        return;
    }
    case 0x06: {                                                   /* ctc1 */
        u32 v = ms->r[rt], f = ms->fcsr;
        switch (fs) {
        case 25: f = (f & ~0xfe800000u) | (v & 0xfe) << 24 | (v & 1) << 23; break;
        case 26: f = (f & ~0x0003f07cu) | (v & 0x0003f07cu); break;
        case 28: f = (f & ~(0xf83u | FCSR_FS)) | (v & 0xf83u) | (v & 4 ? FCSR_FS : 0); break;
        case 31: f = (v & ~FCSR_2008 & ~0x007c0000u) | (f & FCSR_2008); break;
        default: mips_raise(EXC_RI); return;
        }
        ms->fcsr = f;
        /* a cause written with its enable set is the exception, as on
         * the part */
        if (((f >> FCSR_CAUSE_SH) & ((f >> FCSR_ENABLE_SH) | 0x20) & 0x3f))
            mips_raise(EXC_FPE);
        return;
    }
    case 0x08:                                                     /* bc1f, bc1t (l) */
        mips_branch(fcc((i >> 18) & 7) == (int)((i >> 16) & 1),
                    ms->pc + 4 + (u32)(imm << 2), (i >> 17) & 1);
        return;
    case 0x10: arith(0, i & 0x3f, rt, fs, fd, i); return;          /* .s */
    case 0x11: arith(1, i & 0x3f, rt, fs, fd, i); return;          /* .d */
    case 0x14: from_int(0, i & 0x3f, fs, fd); return;              /* .w */
    case 0x15: from_int(1, i & 0x3f, fs, fd); return;              /* .l */
    }
    mips_raise(EXC_RI);
}
