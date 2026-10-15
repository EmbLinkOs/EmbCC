/* riscv-fpu.c -- the RISC-V F and D extensions.
 *
 * The arithmetic is IEEE 754's, done here in integers rather than by the
 * host, so a result and its flags are the same on every host and with
 * any compiler: the five rounding modes, the accrued flags (fflags), the
 * tininess RISC-V detects after rounding, and the architecture's NaNs --
 * every NaN an operation produces is the canonical one, and a single in
 * a 64-bit register is NaN-boxed (its upper half all ones; anything else
 * reads as the canonical NaN). FMIN and FMAX are IEEE 754-2019's
 * minimumNumber and maximumNumber, as QEMU's are.
 *
 * The arithmetic itself is ieee754.c's, which the MIPS FPU shares; this
 * file is the registers and the instructions. */
#include <string.h>

#include "ieee754.h"
#include "riscv.h"

/* ---- the registers --------------------------------------------------------- */

static u64 rd_s(u32 r)
{
    u64 v = rs->f[r];
    return (v >> 32) == 0xffffffffu ? (u32)v : 0x7fc00000u;
}

static u64 rd_f(int d, u32 r)
{
    return d ? rs->f[r] : rd_s(r);
}

static void wr_f(int d, u32 r, u64 v)
{
    rs->f[r] = d ? v : 0xffffffff00000000ull | (u32)v;
    rv_fp_dirty();
}

/* the rounding mode of an instruction, or -1 for an illegal one */
static int rmode(u32 f3)
{
    int rm = (int)(f3 == 7 ? rs->frm : f3);
    return rm > 4 ? -1 : rm;
}

static void flags(u32 fl)
{
    if (fl) {
        rs->fflags |= fl;
        rv_fp_dirty();
    }
}

/* ---- the instructions ------------------------------------------------------- */

int rv_fp_exec(u32 i)
{
    u32 op = i & 0x7f, rd = (i >> 7) & 31, f3 = (i >> 12) & 7;
    u32 r1 = (i >> 15) & 31, r2 = (i >> 20) & 31, f7 = i >> 25;
    int rv64 = rs->xlen == 64;
    u32 fl = 0;
    if (op != 0x07 && op != 0x27 && op != 0x43 && op != 0x47 &&
        op != 0x4b && op != 0x4f && op != 0x53)
        return 0;
    if (!(rs->mstatus & MSTATUS_FS)) {
        rv_illegal();
        return 1;
    }
    if (op == 0x07 || op == 0x27) {             /* FLW FLD FSW FSD */
        if (f3 != 2 && f3 != 3) {
            rv_illegal();
            return 1;
        }
        int n = f3 == 2 ? 4 : 8;
        if (op == 0x07) {
            u64 a = rv_xl(rs->x[r1] + (u64)(s64)((s32)i >> 20));
            u64 v = rv_load(a, n);
            if (!rs->trap)
                wr_f(f3 == 3, rd, v);
        } else {
            s64 imm = (s64)(((s32)i >> 25) << 5) | ((i >> 7) & 31);
            rv_store(rv_xl(rs->x[r1] + (u64)imm), n, rs->f[r2]);
        }
        return 1;
    }
    int fmt = (int)(f7 & 3);
    if (fmt > 1) {
        rv_illegal();
        return 1;
    }
    int d = fmt;
    if (op != 0x53) {                           /* the fused forms */
        int rm = rmode(f3);
        if (rm < 0) {
            rv_illegal();
            return 1;
        }
        u64 r = ieee_fma(d, rd_f(d, r1), rd_f(d, r2), rd_f(d, i >> 27),
                      op == 0x4b || op == 0x4f, op == 0x47 || op == 0x4f, rm,
                      &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    }
    u64 a = rd_f(d, r1), b = rd_f(d, r2), r;
    u32 f5 = f7 >> 2;
    int rm = rmode(f3);
    switch (f5) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x0b:
        if (rm < 0 || (f5 == 0x0b && r2)) {
            rv_illegal();
            return 1;
        }
        r = f5 == 0x00 ? ieee_add(d, a, b, 0, rm, &fl)
          : f5 == 0x01 ? ieee_add(d, a, b, 1, rm, &fl)
          : f5 == 0x02 ? ieee_mul(d, a, b, rm, &fl)
          : f5 == 0x03 ? ieee_div(d, a, b, rm, &fl) : ieee_sqrt(d, a, rm, &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    case 0x04: {                                /* FSGNJ, FSGNJN, FSGNJX */
        u64 sb = (u64)1 << (d ? 63 : 31), s;
        if (f3 > 2) {
            rv_illegal();
            return 1;
        }
        s = f3 == 0 ? b & sb : f3 == 1 ? ~b & sb : (a ^ b) & sb;
        wr_f(d, rd, (a & ~sb) | s);
        return 1;
    }
    case 0x05:                                  /* FMIN, FMAX */
        if (f3 > 1) {
            rv_illegal();
            return 1;
        }
        r = ieee_minmax(d, a, b, (int)f3, &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    case 0x08:                                  /* FCVT.S.D, FCVT.D.S */
        if (rm < 0 || r2 != (u32)!d) {
            rv_illegal();
            return 1;
        }
        r = ieee_convert(d, rd_f(!d, r1), rm, &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    case 0x14:                                  /* FLE FLT FEQ */
        if (f3 > 2) {
            rv_illegal();
            return 1;
        }
        r = (u64)ieee_cmp(d, a, b, (int)f3, &fl);
        rv_wx(rd, r);
        flags(fl);
        return 1;
    case 0x18:                                  /* FCVT.W[U]/L[U].fmt */
        if (rm < 0 || r2 > 3 || (r2 > 1 && !rv64)) {
            rv_illegal();
            return 1;
        }
        r = ieee_to_int(d, a, r2 > 1 ? 64 : 32, (int)(r2 & 1), rm, &fl);
        rv_wx(rd, r2 > 1 ? r : (u64)(s64)(s32)(u32)r);
        flags(fl);
        return 1;
    case 0x1a:                                  /* FCVT.fmt.W[U]/L[U] */
        if (rm < 0 || r2 > 3 || (r2 > 1 && !rv64)) {
            rv_illegal();
            return 1;
        }
        r = ieee_from_int(d, rs->x[r1], r2 > 1 ? 64 : 32, (int)(r2 & 1), rm, &fl);
        wr_f(d, rd, r);
        flags(fl);
        return 1;
    case 0x1c:                                  /* FMV.X.W/D, FCLASS */
        if (r2 || f3 > 1 || (f3 == 0 && d && !rv64)) {
            rv_illegal();
            return 1;
        }
        if (f3 == 1)
            rv_wx(rd, ieee_class(d, a));
        else
            rv_wx(rd, d ? rs->f[r1] : (u64)(s64)(s32)(u32)rs->f[r1]);
        return 1;
    case 0x1e:                                  /* FMV.W/D.X */
        if (r2 || f3 || (d && !rv64)) {
            rv_illegal();
            return 1;
        }
        wr_f(d, rd, rs->x[r1]);
        return 1;
    }
    rv_illegal();
    return 1;
}
