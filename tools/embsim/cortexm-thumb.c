/* cortexm-thumb.c -- the Thumb instruction set of the Cortex-M:
 * ARMv6-M's, and ARMv7-M's with the DSP multiplies and saturation of
 * ARMv7E-M. The floating-point instructions are cortexm-fpu.c's.
 *
 * Each instruction works on the core executing (cs, cortexm.h) and
 * reports a fault with cm_fault_raise; cortexm.c's step discards its
 * partial work and takes the exception. */
#include <string.h>

#include "cortexm.h"

/* ---- flags, shifts and immediates ------------------------------------ */

static void nz(u32 r)
{
    cs->N = (int)(r >> 31);
    cs->Z = r == 0;
}

static u32 add_c(u32 x, u32 y, int cin, int *c, int *v)
{
    u64 us = (u64)x + y + (u32)cin;
    u32 r = (u32)us;
    *c = (int)(us >> 32);
    *v = (int)((~(x ^ y) & (x ^ r)) >> 31);
    return r;
}

enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR, SH_RRX };

static u32 ror32(u32 v, u32 n)
{
    n &= 31;
    return n ? v >> n | v << (32 - n) : v;
}

static u32 shift_c(u32 v, int t, u32 n, int cin, int *cout)
{
    *cout = cin;
    if (t == SH_RRX) {
        *cout = (int)(v & 1);
        return v >> 1 | (u32)cin << 31;
    }
    if (n == 0)
        return v;
    switch (t) {
    case SH_LSL:
        if (n < 32) {
            *cout = (int)(v >> (32 - n) & 1);
            return v << n;
        }
        *cout = n == 32 ? (int)(v & 1) : 0;
        return 0;
    case SH_LSR:
        if (n < 32) {
            *cout = (int)(v >> (n - 1) & 1);
            return v >> n;
        }
        *cout = n == 32 ? (int)(v >> 31) : 0;
        return 0;
    case SH_ASR:
        if (n < 32) {
            *cout = (int)(v >> (n - 1) & 1);
            return (u32)((s32)v >> n);
        }
        *cout = (int)(v >> 31);
        return (u32)((s32)v >> 31);
    default: {
        u32 r = ror32(v, n);
        *cout = (int)(r >> 31);
        return r;
    }
    }
}

static u32 shift(u32 v, int t, u32 n, int cin)
{
    int c;
    return shift_c(v, t, n, cin, &c);
}

/* DecodeImmShift */
static void imm_shift(int type, u32 imm5, int *t, u32 *n)
{
    *t = type;
    *n = imm5;
    if ((type == SH_LSR || type == SH_ASR) && imm5 == 0)
        *n = 32;
    if (type == SH_ROR && imm5 == 0) {
        *t = SH_RRX;
        *n = 1;
    }
}

static u32 expand_imm_c(u32 imm12, int cin, int *cout)
{
    *cout = cin;
    if ((imm12 >> 10) == 0) {
        u32 b = imm12 & 0xff;
        switch ((imm12 >> 8) & 3) {
        case 0: return b;
        case 1: return b | b << 16;
        case 2: return b << 8 | b << 24;
        default: return b * 0x01010101u;
        }
    }
    u32 r = ror32(0x80 | (imm12 & 0x7f), imm12 >> 7);
    *cout = (int)(r >> 31);
    return r;
}


/* ---- 16-bit instructions --------------------------------------------- */

static void ldst_mult(int rn, u32 list, int load, int wb, int db)
{
    int count = 0;
    for (int i = 0; i < 16; i++)
        count += (int)(list >> i & 1);
    u32 base = cs->R[rn];
    u32 a = db ? base - 4 * (u32)count : base;
    u32 vals[16];
    for (int i = 0; i < 16; i++) {
        if (!(list >> i & 1))
            continue;
        if (load)
            vals[i] = cm_mem_ld(a, 4, 1);
        else
            cm_mem_st(a, 4, cm_reg(i), 1);
        a += 4;
    }
    if (cs->fault_exc)
        return;
    if (wb)
        cm_set_reg(rn, db ? base - 4 * (u32)count : base + 4 * (u32)count);
    if (load) {
        for (int i = 0; i < 15; i++)
            if (list >> i & 1)
                cm_set_reg(i, vals[i]);
        if (list >> 15 & 1)
            cm_bx_to(vals[15]);
    }
}

void cm_exec16(u32 h)
{
    int sf = !cm_in_it();
    int c, v;
    u32 r;
    switch (h >> 12) {
    case 0x0: case 0x1: {
        u32 op = h >> 11 & 3;
        int rd = (int)(h & 7), rm = (int)(h >> 3 & 7);
        if (op != 3) {                  /* LSL, LSR, ASR immediate */
            int t;
            u32 n;
            imm_shift((int)op, h >> 6 & 31, &t, &n);
            r = shift_c(cs->R[rm], t, n, cs->C, &c);
            cs->R[rd] = r;
            if (sf) {
                nz(r);
                cs->C = c;
            }
            return;
        }
        u32 opr = (h & 0x400) ? (h >> 6 & 7) : cs->R[h >> 6 & 7];
        int rn = rm;
        if (h & 0x200)
            r = add_c(cs->R[rn], ~opr, 1, &c, &v);
        else
            r = add_c(cs->R[rn], opr, 0, &c, &v);
        cs->R[rd] = r;
        if (sf) {
            nz(r);
            cs->C = c;
            cs->V = v;
        }
        return;
    }
    case 0x2: case 0x3: {               /* MOV CMP ADD SUB imm8 */
        int rd = (int)(h >> 8 & 7);
        u32 imm = h & 0xff;
        switch (h >> 11 & 3) {
        case 0:
            cs->R[rd] = imm;
            if (sf)
                nz(imm);
            return;
        case 1:
            r = add_c(cs->R[rd], ~imm, 1, &c, &v);
            nz(r);
            cs->C = c;
            cs->V = v;
            return;
        case 2:
            r = add_c(cs->R[rd], imm, 0, &c, &v);
            break;
        default:
            r = add_c(cs->R[rd], ~imm, 1, &c, &v);
            break;
        }
        cs->R[rd] = r;
        if (sf) {
            nz(r);
            cs->C = c;
            cs->V = v;
        }
        return;
    }
    case 0x4:
        if ((h >> 10) == 0x10) {        /* data processing */
            int rdn = (int)(h & 7), rm = (int)(h >> 3 & 7);
            u32 a = cs->R[rdn], b = cs->R[rm];
            int lc = cs->C;
            switch (h >> 6 & 15) {
            case 0x0: r = a & b; break;
            case 0x1: r = a ^ b; break;
            case 0x2: r = shift_c(a, SH_LSL, b & 0xff, cs->C, &lc); break;
            case 0x3: r = shift_c(a, SH_LSR, b & 0xff, cs->C, &lc); break;
            case 0x4: r = shift_c(a, SH_ASR, b & 0xff, cs->C, &lc); break;
            case 0x5:
                r = add_c(a, b, cs->C, &c, &v);
                cs->R[rdn] = r;
                if (sf) { nz(r); cs->C = c; cs->V = v; }
                return;
            case 0x6:
                r = add_c(a, ~b, cs->C, &c, &v);
                cs->R[rdn] = r;
                if (sf) { nz(r); cs->C = c; cs->V = v; }
                return;
            case 0x7: r = shift_c(a, SH_ROR, b & 0xff, cs->C, &lc); break;
            case 0x8:
                nz(a & b);
                return;
            case 0x9:                   /* RSB #0 */
                r = add_c(~b, 0, 1, &c, &v);
                cs->R[rdn] = r;
                if (sf) { nz(r); cs->C = c; cs->V = v; }
                return;
            case 0xa:
                r = add_c(a, ~b, 1, &c, &v);
                nz(r); cs->C = c; cs->V = v;
                return;
            case 0xb:
                r = add_c(a, b, 0, &c, &v);
                nz(r); cs->C = c; cs->V = v;
                return;
            case 0xc: r = a | b; break;
            case 0xd:
                r = a * b;
                cs->R[rdn] = r;
                if (sf)
                    nz(r);
                return;
            case 0xe: r = a & ~b; break;
            default: r = ~b; break;
            }
            cs->R[rdn] = r;
            if (sf) {
                nz(r);
                cs->C = lc;
            }
            return;
        }
        if ((h >> 10) == 0x11) {        /* special data, branch exchange */
            int rdn = (int)((h & 7) | (h >> 4 & 8)), rm = (int)(h >> 3 & 15);
            switch (h >> 8 & 3) {
            case 0:
                cm_set_reg(rdn, cm_reg(rdn) + cm_reg(rm));
                return;
            case 1:
                r = add_c(cm_reg(rdn), ~cm_reg(rm), 1, &c, &v);
                nz(r); cs->C = c; cs->V = v;
                return;
            case 2:
                cm_set_reg(rdn, cm_reg(rm));
                return;
            default:
                if (h & 0x80) {         /* BLX */
                    u32 t = cm_reg(rm);
                    cs->R[14] = (cs->pc + 2) | 1;
                    cm_bx_to(t);
                } else
                    cm_bx_to(cm_reg(rm));
                return;
            }
        }
        /* LDR literal */
        cs->R[h >> 8 & 7] = cm_mem_ld(((cs->pc + 4) & ~3u) + (h & 0xff) * 4, 4, 0);
        return;
    case 0x5: {                         /* load/store register offset */
        int rt = (int)(h & 7);
        u32 a = cs->R[h >> 3 & 7] + cs->R[h >> 6 & 7];
        switch (h >> 9 & 7) {
        case 0: cm_mem_st(a, 4, cs->R[rt], 0); return;
        case 1: cm_mem_st(a, 2, cs->R[rt], 0); return;
        case 2: cm_mem_st(a, 1, cs->R[rt], 0); return;
        case 3: r = (u32)(s32)(int8_t)cm_mem_ld(a, 1, 0); break;
        case 4: r = cm_mem_ld(a, 4, 0); break;
        case 5: r = cm_mem_ld(a, 2, 0); break;
        case 6: r = cm_mem_ld(a, 1, 0); break;
        default: r = (u32)(s32)(int16_t)cm_mem_ld(a, 2, 0); break;
        }
        if (!cs->fault_exc)
            cs->R[rt] = r;
        return;
    }
    case 0x6: case 0x7: {               /* STR LDR STRB LDRB imm5 */
        int rt = (int)(h & 7), byte = (int)(h >> 12 & 1);
        u32 a = cs->R[h >> 3 & 7] + (h >> 6 & 31) * (byte ? 1u : 4u);
        if (h & 0x800) {
            r = cm_mem_ld(a, byte ? 1 : 4, 0);
            if (!cs->fault_exc)
                cs->R[rt] = r;
        } else
            cm_mem_st(a, byte ? 1 : 4, cs->R[rt], 0);
        return;
    }
    case 0x8: {                         /* STRH LDRH imm5 */
        int rt = (int)(h & 7);
        u32 a = cs->R[h >> 3 & 7] + (h >> 6 & 31) * 2;
        if (h & 0x800) {
            r = cm_mem_ld(a, 2, 0);
            if (!cs->fault_exc)
                cs->R[rt] = r;
        } else
            cm_mem_st(a, 2, cs->R[rt], 0);
        return;
    }
    case 0x9: {                         /* STR LDR SP-relative */
        int rt = (int)(h >> 8 & 7);
        u32 a = cs->R[13] + (h & 0xff) * 4;
        if (h & 0x800) {
            r = cm_mem_ld(a, 4, 0);
            if (!cs->fault_exc)
                cs->R[rt] = r;
        } else
            cm_mem_st(a, 4, cs->R[rt], 0);
        return;
    }
    case 0xa:                           /* ADR, ADD SP */
        if (h & 0x800)
            cs->R[h >> 8 & 7] = cs->R[13] + (h & 0xff) * 4;
        else
            cs->R[h >> 8 & 7] = ((cs->pc + 4) & ~3u) + (h & 0xff) * 4;
        return;
    case 0xb:
        if ((h & 0xff00) == 0xb000) {   /* ADD, SUB SP imm7 */
            if (h & 0x80)
                cm_set_sp(cs->R[13] - (h & 0x7f) * 4);
            else
                cm_set_sp(cs->R[13] + (h & 0x7f) * 4);
            return;
        }
        if ((h & 0xf500) == 0xb100) {   /* CBZ, CBNZ */
            if (cs->model->arch == ARCH_V6M) {
                cm_undef();
                return;
            }
            u32 off = (h >> 3 & 31) << 1 | (h >> 9 & 1) << 6;
            int nzero = cs->R[h & 7] != 0;
            if (nzero == (int)(h >> 11 & 1))
                cm_branch_to(cs->pc + 4 + off);
            return;
        }
        if ((h & 0xff00) == 0xb200) {   /* SXTH SXTB UXTH UXTB */
            u32 m = cs->R[h >> 3 & 7];
            switch (h >> 6 & 3) {
            case 0: r = (u32)(s32)(int16_t)m; break;
            case 1: r = (u32)(s32)(int8_t)m; break;
            case 2: r = m & 0xffff; break;
            default: r = m & 0xff; break;
            }
            cs->R[h & 7] = r;
            return;
        }
        if ((h & 0xfe00) == 0xb400) {   /* PUSH */
            u32 list = (h & 0xff) | (h & 0x100) << 6;
            ldst_mult(13, list, 0, 1, 1);
            return;
        }
        if ((h & 0xfe00) == 0xbc00) {   /* POP */
            u32 list = (h & 0xff) | (h & 0x100) << 7;
            ldst_mult(13, list, 1, 1, 0);
            return;
        }
        if ((h & 0xffe8) == 0xb660) {   /* CPS */
            if (!cm_privileged())
                return;
            if (h & 0x10) {             /* CPSID: mask */
                if (h & 2) cs->primask = 1;
                if ((h & 1) && cm_exec_prio() > -1) cs->faultmask = 1;
            } else {                    /* CPSIE */
                if (h & 2) cs->primask = 0;
                if (h & 1) cs->faultmask = 0;
            }
            return;
        }
        if ((h & 0xff00) == 0xba00) {   /* REV REV16 REVSH */
            u32 m = cs->R[h >> 3 & 7];
            switch (h >> 6 & 3) {
            case 0:
                r = m >> 24 | (m >> 8 & 0xff00) | (m << 8 & 0xff0000) | m << 24;
                break;
            case 1:
                r = (m >> 8 & 0x00ff00ffu) | (m << 8 & 0xff00ff00u);
                break;
            case 3:
                r = (u32)(s32)(int16_t)((m >> 8 & 0xff) | (m & 0xff) << 8);
                break;
            default:
                cm_undef();
                return;
            }
            cs->R[h & 7] = r;
            return;
        }
        if ((h & 0xff00) == 0xbe00) {   /* BKPT */
            if (cs->sim->semihosting && (h & 0xff) == 0xab) {
                cm_semihost();
                return;
            }
            if (cs->model->arch == ARCH_V7M) {
                cs->hfsr |= 1u << 31;       /* DEBUGEVT */
            }
            cm_fault_raise(EXC_HARD, 0);
            return;
        }
        if ((h & 0xff00) == 0xbf00) {   /* IT, hints */
            if (h & 15) {
                if (cs->model->arch == ARCH_V6M) {
                    cm_undef();
                    return;
                }
                cs->itstate = h & 0xff;
                return;
            }
            u32 hint = h >> 4 & 15;
            if (hint == 2 || hint == 3)
                cm_wfx_idle();
            return;
        }
        cm_undef();
        return;
    case 0xc: {                         /* STM, LDM */
        int rn = (int)(h >> 8 & 7);
        u32 list = h & 0xff;
        if (h & 0x800)
            ldst_mult(rn, list, 1, !(list >> rn & 1), 0);
        else
            ldst_mult(rn, list, 0, 1, 0);
        return;
    }
    case 0xd: {
        u32 cond = h >> 8 & 15;
        if (cond == 14) {
            cm_undef();
            return;
        }
        if (cond == 15) {               /* SVC */
            cm_fault_raise(EXC_SVC, 0);
            return;
        }
        if (cm_cond_passed(cond))
            cm_branch_to(cs->pc + 4 + (u32)((s32)(int8_t)(h & 0xff) * 2));
        return;
    }
    case 0xe:
        if (!(h & 0x800)) {
            u32 off = (h & 0x7ff) << 1;
            if (off & 0x800)
                off |= 0xfffff000u;
            cm_branch_to(cs->pc + 4 + off);
            return;
        }
        cm_undef();
        return;
    }
    cm_undef();
}

/* ---- 32-bit instructions --------------------------------------------- */

/* the data-processing operations of the modified-immediate and
 * shifted-register forms, which share their opcodes */
static void dp_op(u32 op, int s, int rn, int rd, u32 b, int sc)
{
    u32 a = rn == 15 ? 0 : cm_reg(rn), r;
    int c = cs->C, v = cs->V, logical = 1, test = 0;
    switch (op) {
    case 0x0:                           /* AND, TST */
        r = a & b;
        test = rd == 15;
        break;
    case 0x1: r = a & ~b; break;
    case 0x2: r = rn == 15 ? b : a | b; break;   /* ORR, MOV */
    case 0x3: r = rn == 15 ? ~b : a | ~b; break; /* ORN, MVN */
    case 0x4:                           /* EOR, TEQ */
        r = a ^ b;
        test = rd == 15;
        break;
    case 0x8:                           /* ADD, CMN */
        r = add_c(a, b, 0, &c, &v);
        logical = 0;
        test = rd == 15;
        break;
    case 0xa: r = add_c(a, b, cs->C, &c, &v); logical = 0; break;
    case 0xb: r = add_c(a, ~b, cs->C, &c, &v); logical = 0; break;
    case 0xd:                           /* SUB, CMP */
        r = add_c(a, ~b, 1, &c, &v);
        logical = 0;
        test = rd == 15;
        break;
    case 0xe: r = add_c(b, ~a, 1, &c, &v); logical = 0; break;
    default:
        cm_undef();
        return;
    }
    if (test && !s) {
        cm_undef();
        return;
    }
    if (!test)
        cm_set_reg(rd, r);
    if (s) {
        nz(r);
        if (logical)
            cs->C = sc;
        else {
            cs->C = c;
            cs->V = v;
        }
    }
}

static u32 sat_signed(s64 x, u32 bits, int *sat)
{
    s64 hi = ((s64)1 << (bits - 1)) - 1, lo = -((s64)1 << (bits - 1));
    *sat = 0;
    if (x > hi) { *sat = 1; return (u32)hi; }
    if (x < lo) { *sat = 1; return (u32)lo; }
    return (u32)x;
}

static u32 sat_unsigned(s64 x, u32 bits, int *sat)
{
    s64 hi = bits >= 32 ? (s64)0xffffffffll : ((s64)1 << bits) - 1;
    *sat = 0;
    if (x > hi) { *sat = 1; return (u32)hi; }
    if (x < 0) { *sat = 1; return 0; }
    return (u32)x;
}

static void plain_imm(u32 h1, u32 h2)
{
    u32 op = h1 >> 4 & 0x1f;
    int rn = (int)(h1 & 15), rd = (int)(h2 >> 8 & 15);
    u32 imm12 = (h1 >> 10 & 1) << 11 | (h2 >> 12 & 7) << 8 | (h2 & 0xff);
    u32 imm16 = (h1 & 15) << 12 | imm12;
    u32 lsb = (h2 >> 12 & 7) << 2 | (h2 >> 6 & 3);
    u32 w = h2 & 31;
    int sat;
    switch (op) {
    case 0x00:                          /* ADDW, ADR */
        cm_set_reg(rd, (rn == 15 ? (cs->pc + 4) & ~3u : cm_reg(rn)) + imm12);
        return;
    case 0x0a:                          /* SUBW, ADR */
        cm_set_reg(rd, (rn == 15 ? (cs->pc + 4) & ~3u : cm_reg(rn)) - imm12);
        return;
    case 0x04: cm_set_reg(rd, imm16); return;
    case 0x0c: cm_set_reg(rd, (cs->R[rd] & 0xffff) | imm16 << 16); return;
    case 0x10: case 0x12: {             /* SSAT */
        int t = (h1 >> 5 & 1) ? SH_ASR : SH_LSL;
        u32 n = lsb;
        if (t == SH_ASR && n == 0) {
            cm_undef();                    /* SSAT16 */
            return;
        }
        s32 x = (s32)shift(cs->R[rn], t, n, cs->C);
        cs->R[rd] = sat_signed(x, w + 1, &sat);
        if (sat)
            cs->Q = 1;
        return;
    }
    case 0x18: case 0x1a: {             /* USAT */
        int t = (h1 >> 5 & 1) ? SH_ASR : SH_LSL;
        if (t == SH_ASR && lsb == 0) {
            cm_undef();
            return;
        }
        s32 x = (s32)shift(cs->R[rn], t, lsb, cs->C);
        cs->R[rd] = sat_unsigned(x, w, &sat);
        if (sat)
            cs->Q = 1;
        return;
    }
    case 0x14: {                        /* SBFX */
        u32 x = cs->R[rn] >> lsb;
        u32 width = w + 1;
        if (lsb + width > 32) { cm_undef(); return; }
        cs->R[rd] = width == 32 ? x : (u32)((s32)(x << (32 - width)) >> (32 - width));
        return;
    }
    case 0x1c: {                        /* UBFX */
        u32 width = w + 1;
        if (lsb + width > 32) { cm_undef(); return; }
        cs->R[rd] = width == 32 ? cs->R[rn] >> lsb : (cs->R[rn] >> lsb) & ((1u << width) - 1);
        return;
    }
    case 0x16: {                        /* BFI, BFC */
        u32 msb = w;
        if (msb < lsb) { cm_undef(); return; }
        u32 width = msb - lsb + 1;
        u32 mask = (width == 32 ? 0xffffffffu : ((1u << width) - 1)) << lsb;
        u32 src = rn == 15 ? 0 : cs->R[rn] << lsb;
        cs->R[rd] = (cs->R[rd] & ~mask) | (src & mask);
        return;
    }
    }
    cm_undef();
}

static void msr(u32 h1, u32 h2)
{
    u32 v = cs->R[h1 & 15], sysm = h2 & 0xff, mask = h2 >> 10 & 3;
    if (sysm < 8) {
        if (!(sysm & 4)) {
            if (mask & 2)
                cm_set_apsr(v);
            if ((mask & 1) && cs->model->dsp)
                cs->GE = v >> 16 & 15;
        }
        return;
    }
    if (!cm_privileged())
        return;
    switch (sysm) {
    case 8: cm_set_msp(v); return;
    case 9: cm_set_psp(v); return;
    case 16: cs->primask = v & 1; return;
    case 17: if (cs->model->arch == ARCH_V7M) cs->basepri = v & 0xff; return;
    case 18:
        if (cs->model->arch == ARCH_V7M && (v & 0xff) &&
            ((v & 0xff) < cs->basepri || cs->basepri == 0))
            cs->basepri = v & 0xff;
        return;
    case 19:
        if (cs->model->arch == ARCH_V7M && cm_exec_prio() > -1)
            cs->faultmask = v & 1;
        return;
    case 20:
        cs->control = (cs->control & ~1u) | (v & 1);
        if (cs->model->arch == ARCH_V6M)
            cs->control &= ~1u;
        if (!cs->ipsr) {
            cs->control = (cs->control & ~2u) | (v & 2);
            cm_use_psp((cs->control & 2) != 0);
        }
        if (cs->model->fpu)
            cs->control = (cs->control & ~4u) | (v & 4);
        return;
    }
}

static u32 mrs(u32 h2)
{
    u32 sysm = h2 & 0xff, v = 0;
    if (sysm < 8) {
        if (sysm & 1)
            v |= cs->ipsr & 0x1ff;
        if (!(sysm & 4)) {
            v |= (u32)cs->N << 31 | (u32)cs->Z << 30 | (u32)cs->C << 29 | (u32)cs->V << 28 |
                 (u32)cs->Q << 27;
            if (cs->model->dsp)
                v |= cs->GE << 16;
        }
        return v;
    }
    if (!cm_privileged() && sysm != 20)
        return 0;
    switch (sysm) {
    case 8: return cm_get_msp();
    case 9: return cm_get_psp();
    case 16: return cs->primask;
    case 17: case 18: return cs->basepri;
    case 19: return cs->faultmask;
    case 20: return cs->control & (cs->model->fpu ? 7u : 3u);
    }
    return 0;
}

static void branch_misc(u32 h1, u32 h2)
{
    u32 op1 = h2 >> 12 & 5;
    u32 S_ = h1 >> 10 & 1, J1 = h2 >> 13 & 1, J2 = h2 >> 11 & 1;
    if (op1 == 5 || op1 == 1) {         /* BL, B.W */
        if (cs->model->arch == ARCH_V6M && op1 == 1) {
            cm_undef();
            return;
        }
        u32 I1 = !(J1 ^ S_), I2 = !(J2 ^ S_);
        u32 off = S_ << 24 | I1 << 23 | I2 << 22 | (h1 & 0x3ff) << 12 |
                  (h2 & 0x7ff) << 1;
        if (S_)
            off |= 0xfe000000u;
        if (op1 == 5)
            cs->R[14] = (cs->pc + 4) | 1;
        cm_branch_to(cs->pc + 4 + off);
        return;
    }
    if (op1 == 4) {                     /* BLX imm: no ARM state */
        cm_undef();
        return;
    }
    u32 op = h1 >> 4 & 0x7f;
    if ((op & 0x38) != 0x38) {          /* B<c>.W */
        if (cs->model->arch == ARCH_V6M) {
            cm_undef();
            return;
        }
        u32 off = S_ << 20 | J2 << 19 | J1 << 18 | (h1 & 0x3f) << 12 |
                  (h2 & 0x7ff) << 1;
        if (S_)
            off |= 0xffe00000u;
        if (cm_cond_passed(h1 >> 6 & 15))
            cm_branch_to(cs->pc + 4 + off);
        return;
    }
    if ((h2 >> 12 & 7) == 2 && op == 0x7f) {
        cm_undef();                        /* UDF.W */
        return;
    }
    if (op1 != 0) {
        cm_undef();
        return;
    }
    switch (op) {
    case 0x38: case 0x39:
        msr(h1, h2);
        return;
    case 0x3a:                          /* hints */
        if (cs->model->arch == ARCH_V6M) {
            cm_undef();
            return;
        }
        if ((h2 & 0xff) == 2 || (h2 & 0xff) == 3)
            cm_wfx_idle();
        return;
    case 0x3b:                          /* CLREX, DSB, DMB, ISB */
        if ((h2 >> 4 & 15) == 2) {
            if (cs->model->arch == ARCH_V6M) {
                cm_undef();
                return;
            }
            cs->excl_valid = 0;
        }
        return;
    case 0x3e: case 0x3f:
        cm_set_reg((int)(h2 >> 8 & 15), mrs(h2));
        return;
    }
    cm_undef();
}

/* LDR, LDRB, LDRH, LDRSB, LDRSH, STR, STRB, STRH: every 32-bit form */
static void ldst_single(u32 h1, u32 h2)
{
    int load = (int)(h1 >> 4 & 1), size = (int)(h1 >> 5 & 3);
    int sgn = (int)(h1 >> 8 & 1), rn = (int)(h1 & 15);
    int rt = (int)(h2 >> 12 & 15);
    int n = size == 0 ? 1 : size == 1 ? 2 : 4;
    u32 a, base;
    int wb = 0;
    if (size == 3 || (!load && sgn)) {
        cm_undef();
        return;
    }
    if (rn == 15) {                     /* literal */
        if (!load) {
            cm_undef();
            return;
        }
        u32 imm = h2 & 0xfff;
        base = (cs->pc + 4) & ~3u;
        a = (h1 & 0x80) ? base + imm : base - imm;
    } else if (h1 & 0x80) {             /* imm12 */
        base = cs->R[rn];
        a = base + (h2 & 0xfff);
    } else if ((h2 & 0x800)) {          /* imm8: P U W */
        int P = (int)(h2 >> 10 & 1), U = (int)(h2 >> 9 & 1);
        int W = (int)(h2 >> 8 & 1);
        u32 imm = h2 & 0xff;
        base = cs->R[rn];
        u32 off = U ? base + imm : base - imm;
        a = P ? off : base;
        if (W) {
            wb = 1;
            base = off;
        } else if (!P) {
            cm_undef();
            return;
        }
    } else if ((h2 & 0xfc0) == 0) {     /* register */
        base = cs->R[rn];
        a = base + (cs->R[h2 & 15] << (h2 >> 4 & 3));
    } else {
        cm_undef();
        return;
    }
    if (load && rt == 15 && n != 4)
        return;                         /* PLD, PLI, and other hints */
    if (load) {
        u32 v = cm_mem_ld(a, n, 0);
        if (sgn)
            v = n == 1 ? (u32)(s32)(int8_t)v : (u32)(s32)(int16_t)v;
        if (cs->fault_exc)
            return;
        if (wb)
            cm_set_reg(rn, base);
        if (rt == 15) {
            if (a & 3) {
                cm_fault_raise(EXC_USAGE, UFSR_UNALIGNED);
                return;
            }
            cm_bx_to(v);
        } else
            cm_set_reg(rt, v);
    } else {
        cm_mem_st(a, n, cm_reg(rt), 0);
        if (!cs->fault_exc && wb)
            cm_set_reg(rn, base);
    }
}

static void ldst_dual_excl(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 7 & 3, op2 = h1 >> 4 & 3, op3 = h2 >> 4 & 15;
    int rn = (int)(h1 & 15), rt = (int)(h2 >> 12 & 15);
    int rt2 = (int)(h2 >> 8 & 15), rd = (int)(h2 & 15);
    if ((op1 & 2) || (op2 & 2)) {       /* LDRD, STRD */
        int P = (int)(h1 >> 8 & 1), U = (int)(h1 >> 7 & 1);
        int W = (int)(h1 >> 5 & 1), L = (int)(h1 >> 4 & 1);
        u32 imm = (h2 & 0xff) * 4;
        u32 base = rn == 15 ? (cs->pc + 4) & ~3u : cs->R[rn];
        u32 off = U ? base + imm : base - imm;
        u32 a = P ? off : base;
        if (L) {
            u32 lo = cm_mem_ld(a, 4, 1), hi = cm_mem_ld(a + 4, 4, 1);
            if (cs->fault_exc)
                return;
            if (W)
                cm_set_reg(rn, off);
            cm_set_reg(rt, lo);
            cm_set_reg(rt2, hi);
        } else {
            cm_mem_st(a, 4, cs->R[rt], 1);
            cm_mem_st(a + 4, 4, cs->R[rt2], 1);
            if (!cs->fault_exc && W)
                cm_set_reg(rn, off);
        }
        return;
    }
    if (op1 == 0) {
        u32 a = cs->R[rn] + (h2 & 0xff) * 4;
        if (op2 == 1) {                 /* LDREX */
            u32 v = cm_mem_ld(a, 4, 1);
            if (cs->fault_exc)
                return;
            cm_set_reg(rt, v);
            cs->excl_valid = 1;
            cs->excl_addr = a;
        } else {                        /* STREX: Rd is bits 11:8 */
            int ok = cs->excl_valid && cs->excl_addr == a;
            if (!cm_aligned_ok(a, 4, 1))
                return;
            if (ok)
                cm_st(a, 4, cs->R[rt]);
            if (cs->fault_exc)
                return;
            cs->excl_valid = 0;
            cm_set_reg(rt2, ok ? 0 : 1);
        }
        return;
    }
    /* op1 == 1 */
    if (op2 == 1 && (op3 == 0 || op3 == 1)) {   /* TBB, TBH */
        u32 base = rn == 15 ? cs->pc + 4 : cs->R[rn];
        u32 off = op3 ? 2 * cm_mem_ld(base + 2 * cs->R[rd], 2, 0)
                      : 2 * cm_mem_ld(base + cs->R[rd], 1, 0);
        if (!cs->fault_exc)
            cm_branch_to(cs->pc + 4 + off);
        return;
    }
    if (op3 == 4 || op3 == 5) {         /* LDREXB/H, STREXB/H */
        int n = op3 == 4 ? 1 : 2;
        u32 a = cs->R[rn];
        if (op2 == 1) {
            u32 v = cm_mem_ld(a, n, 1);
            if (cs->fault_exc)
                return;
            cm_set_reg(rt, v);
            cs->excl_valid = 1;
            cs->excl_addr = a;
        } else {
            int ok = cs->excl_valid && cs->excl_addr == a;
            if (!cm_aligned_ok(a, n, 1))
                return;
            if (ok)
                cm_st(a, n, cs->R[rt]);
            if (cs->fault_exc)
                return;
            cs->excl_valid = 0;
            cm_set_reg(rd, ok ? 0 : 1);
        }
        return;
    }
    cm_undef();
}

static void dp_register(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 4 & 15, op2 = h2 >> 4 & 15;
    int rn = (int)(h1 & 15), rd = (int)(h2 >> 8 & 15), rm = (int)(h2 & 15);
    if ((h2 & 0xf000) != 0xf000) {
        cm_undef();
        return;
    }
    if (op2 == 0 && op1 < 8) {          /* LSL LSR ASR ROR register */
        int c;
        u32 r = shift_c(cs->R[rn], (int)(op1 >> 1), cs->R[rm] & 0xff, cs->C, &c);
        cm_set_reg(rd, r);
        if (op1 & 1) {
            nz(r);
            cs->C = c;
        }
        return;
    }
    if (op1 < 6 && (op2 & 8)) {         /* extend, and extend and add */
        u32 x = ror32(cs->R[rm], 8 * (op2 & 3)), r;
        switch (op1) {
        case 0: r = (u32)(s32)(int16_t)x; break;
        case 1: r = x & 0xffff; break;
        case 4: r = (u32)(s32)(int8_t)x; break;
        case 5: r = x & 0xff; break;
        default:
            cm_undef();                    /* SXTB16, UXTB16 */
            return;
        }
        if (rn != 15)
            r += cs->R[rn];
        cm_set_reg(rd, r);
        return;
    }
    if (op1 >= 8 && op1 < 12 && (op2 & 0xc) == 8) {
        u32 m = cs->R[rm], r;
        int sat;
        switch (op1 << 4 | (op2 & 3)) {
        case 0x80: case 0x81: case 0x82: case 0x83: {   /* QADD etc */
            if (!cs->model->dsp) {
                cm_undef();
                return;
            }
            s64 n = (s32)cs->R[rn], mm = (s32)m;
            int s1 = 0;
            if (op2 & 1) {              /* QDADD, QDSUB double Rn */
                n = (s32)sat_signed(2 * n, 32, &s1);
            }
            s64 x = (op2 & 2) ? mm - n : mm + n;
            r = sat_signed(x, 32, &sat);
            if (sat || s1)
                cs->Q = 1;
            break;
        }
        case 0x90:
            r = m >> 24 | (m >> 8 & 0xff00) | (m << 8 & 0xff0000) | m << 24;
            break;
        case 0x91:
            r = (m >> 8 & 0x00ff00ffu) | (m << 8 & 0xff00ff00u);
            break;
        case 0x92:
            r = 0;
            for (int i = 0; i < 32; i++)
                r |= (m >> i & 1) << (31 - i);
            break;
        case 0x93:
            r = (u32)(s32)(int16_t)((m >> 8 & 0xff) | (m & 0xff) << 8);
            break;
        case 0xa0: {                    /* SEL */
            if (!cs->model->dsp) {
                cm_undef();
                return;
            }
            u32 n = cs->R[rn];
            r = 0;
            for (int b = 0; b < 4; b++)
                r |= ((cs->GE >> b & 1) ? n : m) & (0xffu << (8 * b));
            break;
        }
        case 0xb0:
            r = 0;
            while (r < 32 && !(m >> (31 - r) & 1))
                r++;
            break;
        default:
            cm_undef();
            return;
        }
        cm_set_reg(rd, r);
        return;
    }
    cm_undef();                            /* parallel add and subtract */
}

static s32 half(u32 v, int top)
{
    return top ? (s32)(int16_t)(v >> 16) : (s32)(int16_t)v;
}

static void multiply(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 4 & 7, op2 = h2 >> 4 & 3;
    int rn = (int)(h1 & 15), ra = (int)(h2 >> 12 & 15);
    int rd = (int)(h2 >> 8 & 15), rm = (int)(h2 & 15);
    u32 a = cs->R[rn], b = cs->R[rm];
    if (op1 == 0) {
        if (op2 == 0)
            cm_set_reg(rd, ra == 15 ? a * b : a * b + cs->R[ra]);
        else if (op2 == 1)
            cm_set_reg(rd, cs->R[ra] - a * b);
        else
            cm_undef();
        return;
    }
    if (!cs->model->dsp) {
        cm_undef();
        return;
    }
    switch (op1) {
    case 1: {                           /* SMULxy, SMLAxy */
        s32 p = half(a, (int)(op2 >> 1 & 1)) * half(b, (int)(op2 & 1));
        if (ra == 15)
            cm_set_reg(rd, (u32)p);
        else {
            s64 r = (s64)p + (s32)cs->R[ra];
            if (r != (s32)r)
                cs->Q = 1;
            cm_set_reg(rd, (u32)r);
        }
        return;
    }
    case 3: {                           /* SMULWy, SMLAWy */
        s64 p = ((s64)(s32)a * half(b, (int)(op2 & 1))) >> 16;
        if (op2 & 2) {
            cm_undef();
            return;
        }
        if (ra == 15)
            cm_set_reg(rd, (u32)p);
        else {
            s64 r = p + (s32)cs->R[ra];
            if (r != (s32)r)
                cs->Q = 1;
            cm_set_reg(rd, (u32)r);
        }
        return;
    }
    case 5: case 6: {                   /* SMMUL, SMMLA, SMMLS */
        s64 p = (s64)(s32)a * (s32)b;
        s64 acc = ra == 15 ? 0 : (s64)((u64)cs->R[ra] << 32);
        s64 r = op1 == 6 ? acc - p : acc + p;
        if (op2 & 1)
            r += 0x80000000ll;
        cm_set_reg(rd, (u32)((u64)r >> 32));
        return;
    }
    }
    cm_undef();
}

static void long_mul_div(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 4 & 7, op2 = h2 >> 4 & 15;
    int rn = (int)(h1 & 15), lo = (int)(h2 >> 12 & 15);
    int hi = (int)(h2 >> 8 & 15), rm = (int)(h2 & 15);
    u32 a = cs->R[rn], b = cs->R[rm];
    if (op1 == 1 || op1 == 3) {         /* SDIV, UDIV */
        if (op2 != 15) {
            cm_undef();
            return;
        }
        u32 r;
        if (b == 0) {
            if (cs->ccr & 0x10) {
                cm_fault_raise(EXC_USAGE, UFSR_DIVBYZERO);
                return;
            }
            r = 0;
        } else if (op1 == 1)
            r = (a == 0x80000000u && b == 0xffffffffu)
                    ? a : (u32)((s32)a / (s32)b);
        else
            r = a / b;
        cm_set_reg(hi, r);
        return;
    }
    u64 acc = (u64)cs->R[hi] << 32 | cs->R[lo], r;
    switch (op1 << 4 | op2) {
    case 0x00: r = (u64)((s64)(s32)a * (s32)b); break;
    case 0x20: r = (u64)a * b; break;
    case 0x40: r = acc + (u64)((s64)(s32)a * (s32)b); break;
    case 0x60: r = acc + (u64)a * b; break;
    case 0x66:                          /* UMAAL */
        if (!cs->model->dsp) {
            cm_undef();
            return;
        }
        r = (u64)a * b + cs->R[lo] + cs->R[hi];
        break;
    default:
        if (cs->model->dsp && op1 == 4 && (op2 & 0xc) == 8) {  /* SMLALxy */
            r = acc + (u64)(s64)(half(a, (int)(op2 >> 1 & 1)) *
                                 half(b, (int)(op2 & 1)));
            break;
        }
        cm_undef();
        return;
    }
    cm_set_reg(lo, (u32)r);
    cm_set_reg(hi, (u32)(r >> 32));
}

void cm_exec32(u32 h1, u32 h2)
{
    u32 op1 = h1 >> 11 & 3;
    if (cs->model->arch == ARCH_V6M) {
        /* BL, MSR, MRS, the barriers and UDF.W are all ARMv6-M has */
        if (op1 != 2 || !(h2 & 0x8000)) {
            cm_undef();
            return;
        }
        branch_misc(h1, h2);
        return;
    }
    if (op1 == 1) {
        if ((h1 & 0x0640) == 0x0000) {  /* load/store multiple */
            u32 op = h1 >> 7 & 3;
            int W = (int)(h1 >> 5 & 1), L = (int)(h1 >> 4 & 1);
            int rn = (int)(h1 & 15);
            if (op == 0 || op == 3) {
                cm_undef();
                return;
            }
            if (L && W && (h2 >> rn & 1))
                W = 0;
            ldst_mult(rn, h2, L, W, op == 2);
            return;
        }
        if ((h1 & 0x0640) == 0x0040) {
            ldst_dual_excl(h1, h2);
            return;
        }
        if ((h1 & 0x0600) == 0x0200) {  /* data processing, shifted reg */
            int t, sc;
            u32 n;
            imm_shift((int)(h2 >> 4 & 3), (h2 >> 12 & 7) << 2 | (h2 >> 6 & 3),
                      &t, &n);
            u32 op = h1 >> 5 & 15;
            if (op == 6) {              /* PKHBT, PKHTB */
                if (!cs->model->dsp) {
                    cm_undef();
                    return;
                }
                u32 m = shift(cs->R[h2 & 15], t, n, cs->C), a = cs->R[h1 & 15], r;
                if (h2 & 0x20)
                    r = (a & 0xffff0000u) | (m & 0xffff);
                else
                    r = (m & 0xffff0000u) | (a & 0xffff);
                cm_set_reg((int)(h2 >> 8 & 15), r);
                return;
            }
            u32 b = shift_c(cm_reg((int)(h2 & 15)), t, n, cs->C, &sc);
            dp_op(op, (int)(h1 >> 4 & 1), (int)(h1 & 15), (int)(h2 >> 8 & 15),
                  b, sc);
            return;
        }
        /* coprocessor: the FPU, cp10 and cp11 */
        if ((h2 >> 9 & 7) != 5) {
            cm_undef();
            return;
        }
        if ((h1 & 0x0e00) == 0x0c00) {
            cm_vfp_ldst(h1, h2);
            return;
        }
        if ((h1 & 0x0f00) == 0x0e00) {
            if (h2 & 0x10)
                cm_vfp_xfer(h1, h2);
            else
                cm_vfp_dp(h1, h2);
            return;
        }
        cm_undef();
        return;
    }
    if (op1 == 2) {
        if (h2 & 0x8000) {
            branch_misc(h1, h2);
            return;
        }
        if (h1 & 0x0200) {
            plain_imm(h1, h2);
            return;
        }
        int sc;
        u32 imm12 = (h1 >> 10 & 1) << 11 | (h2 >> 12 & 7) << 8 | (h2 & 0xff);
        u32 b = expand_imm_c(imm12, cs->C, &sc);
        dp_op(h1 >> 5 & 15, (int)(h1 >> 4 & 1), (int)(h1 & 15),
              (int)(h2 >> 8 & 15), b, sc);
        return;
    }
    /* op1 == 3 */
    if ((h1 & 0x0e00) == 0x0800) {      /* 1111 100x: loads, stores */
        ldst_single(h1, h2);
        return;
    }
    if ((h1 & 0x0f00) == 0x0a00) {
        dp_register(h1, h2);
        return;
    }
    if ((h1 & 0x0f80) == 0x0b00) {
        multiply(h1, h2);
        return;
    }
    if ((h1 & 0x0f80) == 0x0b80) {
        long_mul_div(h1, h2);
        return;
    }
    if ((h1 & 0x0c00) == 0x0c00 && (h2 >> 9 & 7) == 5) {
        /* the FPv5 forms with the T bit (VSEL, VMAXNM, VRINT...): not
         * modelled yet */
        cm_undef();
        return;
    }
    cm_undef();
}
