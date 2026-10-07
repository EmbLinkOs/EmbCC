/* Thumb-2 encoding for ARMv7-M. See emit.h for the model; every encoder
 * here is checked against llvm-objdump by tools/thumbcheck. */
#include <stdio.h>
#include <stdlib.h>

#include "emit.h"
#include "a32.h"

/* ARM state (armv7a-none-eabi): each encoder below hands its call to its
 * A32 twin in a32.c when this is set (docs/internals/arm-a32-plan.md). In
 * Thumb state nothing changes -- the test is the first statement and
 * false -- so the Cortex-M encodings are exactly what they were. */
int t_isa_a32;

#define A32(call) do { if (t_isa_a32) { call; return; } } while (0)

/* A Thumb-only form asked for in ARM state: a code generator bug, since
 * every caller checks t_isa_a32 first. */
static void a32_refuse(const char *what, long v)
{
    fprintf(stderr, "embcc: internal: arm: %s has no A32 form (%ld)\n",
            what, v);
    abort();
}

/* A Thumb instruction is one or two halfwords, each written
 * little-endian. A 32-bit instruction is NOT a little-endian word: its
 * two halfwords are in program order and each is byte-swapped on its
 * own, so `f241 2034` lands in memory as 41 f2 34 20. Writing it as a
 * u32 would reverse the halfwords and is the single easiest way to get
 * this file wrong. */
static void hw(struct code *c, unsigned v)
{
    code_byte(c, (int)(v & 0xff));
    code_byte(c, (int)((v >> 8) & 0xff));
}

static void hw2(struct code *c, unsigned a, unsigned b)
{
    hw(c, a);
    hw(c, b);
}

/* A coprocessor (VFP) instruction: the same 28 bits in both states, under
 * a condition field in ARM state where Thumb has 1110. */
static void vhw2(struct code *c, unsigned a, unsigned b)
{
    if (t_isa_a32)
        a32_vfp_word(c, a, b);
    else
        hw2(c, a, b);
}

static void patch_hw(struct code *c, int off, unsigned v)
{
    c->p[off]     = (unsigned char)(v & 0xff);
    c->p[off + 1] = (unsigned char)((v >> 8) & 0xff);
}

static int low(int r) { return r < 8; }

/* ---- the modified immediate ----------------------------------------
 *
 * ThumbExpandImm (ARMv7-M ARM A5.3.2). The 12 bits are i:imm3:imm8. With
 * i:imm3 below 0b1000 the top two bits of imm3 select one of four
 * replication patterns; otherwise the whole 12 bits are a rotate count
 * applied to imm8 with its top bit forced on.
 *
 * Encoded by SEARCH rather than by inverting the rule: there are 4096
 * encodings, tools/thumbcheck walks every one of them against the
 * assembler, and a search that tries the cheap shapes first cannot
 * produce a value that decodes to something else. Inverting the rule is
 * how the aarch64 bitmask encoder got its rotation backwards. */
static unsigned expand_imm(unsigned e)
{
    unsigned imm8 = e & 0xff;
    /* The selector is bits 11:10 — NOT the whole i:imm3 field. With both
     * clear, bits 9:8 pick one of four replication patterns; otherwise
     * the top five bits are a rotate count, which is why they cannot
     * also be a pattern selector. */
    if ((e >> 10) == 0) {
        switch ((e >> 8) & 3) {
        case 0: return imm8;
        case 1: return (imm8 << 16) | imm8;
        case 2: return (imm8 << 24) | (imm8 << 8);
        default: return (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8;
        }
    }
    {
        unsigned v = 0x80 | (imm8 & 0x7f);
        unsigned rot = e >> 7;              /* 8..31, so never a 32-bit shift */
        return (v >> rot) | (v << (32 - rot));
    }
}

/* Every encoding, ordered by the value it expands to and then by the
 * encoding itself: built once, from expand_imm, so it is still the search
 * that decides -- but a lookup in it is a dozen steps where a walk of all
 * 4096 for every immediate the backend emitted or asked about was one of
 * the larger costs of compiling a big function. */
static unsigned g_imm_val[4096];
static int g_imm_enc[4096];
static int g_imm_ready;

static int imm_order(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    unsigned va = expand_imm((unsigned)a), vb = expand_imm((unsigned)b);
    if (va != vb)
        return va < vb ? -1 : 1;
    return a < b ? -1 : a > b;
}

/* The 12-bit field that expands to `v`, or -1: the lowest one, as a walk
 * from 0 found. */
static int encode_imm(unsigned long v)
{
    unsigned want = (unsigned)(v & 0xffffffffUL);
    if (!g_imm_ready) {
        for (int e = 0; e < 4096; e++)
            g_imm_enc[e] = e;
        qsort(g_imm_enc, 4096, sizeof g_imm_enc[0], imm_order);
        for (int k = 0; k < 4096; k++)
            g_imm_val[k] = expand_imm((unsigned)g_imm_enc[k]);
        g_imm_ready = 1;
    }
    int lo = 0, hi = 4095;
    while (lo < hi) {                     /* the first entry >= want */
        int mid = lo + (hi - lo) / 2;
        if (g_imm_val[mid] < want) lo = mid + 1;
        else hi = mid;
    }
    return g_imm_val[lo] == want ? g_imm_enc[lo] : -1;
}

int t_imm_ok(long imm)
{
    if (t_isa_a32)
        return a32_imm_ok(imm);
    return encode_imm((unsigned long)imm) >= 0;
}

/* i:imm3:imm8 split into the two halfwords' fields. */
static unsigned imm_i(int e)    { return (unsigned)(e >> 11) & 1; }
static unsigned imm_hi3(int e)  { return (unsigned)(e >> 8) & 7; }
static unsigned imm_lo8(int e)  { return (unsigned)e & 0xff; }

/* ---- moves ---------------------------------------------------------- */

void t_mov_reg(struct code *c, int rd, int rm)
{
    A32(a32_mov_reg(c, rd, rm));
    if (rd == rm)
        return;
    hw(c, 0x4600u | (unsigned)((rd & 8) << 4) | (unsigned)(rm << 3) |
           (unsigned)(rd & 7));
}

void t_movs_reg(struct code *c, int rd, int rm)
{
    A32(a32_movs_reg(c, rd, rm));
    if (low(rd) && low(rm))
        hw(c, 0x0000u | (unsigned)(rm << 3) | (unsigned)rd); /* LSLS #0 */
    else
        hw2(c, 0xea5fu, (unsigned)(rd << 8) | (unsigned)rm);   /* MOVS.W */
}

int t_movs_imm(struct code *c, int rd, long imm)
{
    if (t_isa_a32) return a32_movs_imm(c, rd, imm);
    unsigned long v = (unsigned long)imm & 0xffffffffUL;
    int e;
    if (low(rd) && v <= 0xff) {
        hw(c, 0x2000u | (unsigned)(rd << 8) | (unsigned)v);
        return 0;
    }
    e = encode_imm(v);
    if (e < 0)
        return -1;
    hw2(c, 0xf04fu | (imm_i(e) << 10) | (1u << 4),
           (imm_hi3(e) << 12) | (unsigned)(rd << 8) | imm_lo8(e));
    return 0;
}

/* A constant one instruction can move into ANY register without
 * touching the flags: what an IT block's slot can hold (t_mov_imm_it). */
int t_it_imm_ok(long imm)
{
    unsigned long v = (unsigned long)imm & 0xffffffffUL;
    /* A32: one conditional MOV, MVN or MOVW (a32_mov_imm takes the
     * queued condition). Not the Thumb answer -- A32's rotations are
     * not Thumb's modified immediates. */
    if (t_isa_a32)
        return a32_encode_imm(v) >= 0 ||
               a32_encode_imm(~v & 0xffffffffUL) >= 0 || v <= 0xffff;
    return v <= 0xff || encode_imm(v) >= 0 ||
           encode_imm(~v & 0xffffffffUL) >= 0;
}

/* `mov<c> rd, #imm` inside an IT block, one instruction (t_it_imm_ok):
 * the 16-bit MOVS encoding for 0..255 into r0-r7, which an IT block
 * makes MOV<c> and flagless; else MOV.W or MVN.W with a modified
 * immediate, S clear. */
void t_mov_imm_it(struct code *c, int rd, long imm)
{
    A32(a32_mov_imm(c, rd, imm, 0));
    unsigned long v = (unsigned long)imm & 0xffffffffUL;
    int e;
    if (low(rd) && v <= 0xff) {
        hw(c, 0x2000u | (unsigned)(rd << 8) | (unsigned)v);
        return;
    }
    unsigned op = 0xf04fu;                      /* MOV.W */
    e = encode_imm(v);
    if (e < 0) {
        op = 0xf06fu;                           /* MVN.W */
        e = encode_imm(~v & 0xffffffffUL);
    }
    hw2(c, op | (imm_i(e) << 10),
           (imm_hi3(e) << 12) | (unsigned)(rd << 8) | imm_lo8(e));
}

void t_mvn_reg(struct code *c, int rd, int rm, int s)
{
    A32(a32_mvn_reg(c, rd, rm, s));
    if (s && low(rd) && low(rm)) {
        hw(c, 0x43c0u | (unsigned)(rm << 3) | (unsigned)rd);
        return;
    }
    /* MVN is ORN with rn = PC. */
    hw2(c, 0xea6fu | (unsigned)(s << 4),
           (unsigned)(rd << 8) | (unsigned)rm);
}

static void movw(struct code *c, int rd, unsigned v, int top)
{
    unsigned imm4 = (v >> 12) & 0xf, i = (v >> 11) & 1;
    unsigned imm3 = (v >> 8) & 7, imm8 = v & 0xff;
    hw2(c, (top ? 0xf2c0u : 0xf240u) | (i << 10) | imm4,
           (imm3 << 12) | (unsigned)(rd << 8) | imm8);
}

/* movw / movt, exposed for the file assembler: a .S file writes the
 * two halves of an address by hand where the compiler emits them as a
 * relocated pair. */
void t_movw_movt(struct code *c, int rd, unsigned v, int top)
{
    A32(a32_movw_movt(c, rd, v, top));
    movw(c, rd, v, top);
}

/* A constant into a register where THE FLAGS ARE DEAD.
 *
 * `movs rd, #imm` is two bytes for 0..255 into r0-r7, and `movw` is
 * four -- but movs always sets the flags, so t_mov_imm only reaches it
 * when the caller asked for flag-setting, and no caller ever does.
 * movw was the single commonest instruction EmbCC emitted for ARMv7-M.
 *
 * Whether that is safe is a question about this backend, and the
 * answer is written into it: every lowering that tests a value emits
 * its own `cmp` first (see IR_BRZ), so no flag value survives from one
 * IR instruction to the next. A lowering may therefore clobber the
 * flags freely; only a sequence that has already compared inside
 * ITSELF must not, and that one keeps calling t_mov_imm.
 */
void t_mov_imm_dead_flags(struct code *c, int rd, long imm)
{
    t_mov_imm(c, rd, imm, 1);
}

void t_mov_imm(struct code *c, int rd, long imm, int s)
{
    A32(a32_mov_imm(c, rd, imm, s));
    unsigned long v = (unsigned long)imm & 0xffffffffUL;
    if (s && low(rd) && v <= 0xff) {
        hw(c, 0x2000u | (unsigned)(rd << 8) | (unsigned)v);
        return;
    }
    if (v <= 0xffff) {
        movw(c, rd, (unsigned)v, 0);
        return;
    }
    {
        int e = encode_imm(v);
        if (e >= 0) {           /* MOV (immediate), T2 — four bytes, not eight */
            hw2(c, 0xf04fu | (imm_i(e) << 10) | (unsigned)(s << 4),
                   (imm_hi3(e) << 12) | (unsigned)(rd << 8) | imm_lo8(e));
            return;
        }
    }
    /* movw then movt; with the flags dead, a low half of eight bits
     * into r0-r7 is the two-byte movs (a double's low half is often 0) */
    if (s && low(rd) && (v & 0xffff) <= 0xff)
        hw(c, 0x2000u | (unsigned)(rd << 8) | (unsigned)(v & 0xff));
    else
        movw(c, rd, (unsigned)(v & 0xffff), 0);
    movw(c, rd, (unsigned)(v >> 16), 1);
}

int t_mov_addr(struct code *c, int rd, unsigned long value)
{
    if (t_isa_a32) return a32_mov_addr(c, rd, value);
    int at = c->len;
    movw(c, rd, (unsigned)(value & 0xffff), 0);
    movw(c, rd, (unsigned)((value >> 16) & 0xffff), 1);
    return at;
}

/* ---- data processing ------------------------------------------------ */

/* The 16-bit shared data-processing block, 010000 op rm rdn. Indexed by
 * the wide encoding's op so one caller drives both; -1 where the 16-bit
 * block has no member for that operation. */
static int narrow_dp(int op)
{
    switch (op) {
    case T_OP_AND: return 0;
    case T_OP_EOR: return 1;
    case T_OP_ADC: return 5;
    case T_OP_SBC: return 6;
    case T_OP_ORR: return 12;
    case T_OP_BIC: return 14;
    default:       return -1;
    }
}

void t_alu_reg(struct code *c, int op, int rd, int rn, int rm, int s)
{
    A32(a32_alu_reg(c, op, rd, rn, rm, s));
    if (s && low(rd) && low(rn) && low(rm)) {
        /* The three-operand 16-bit adds/subs. */
        if (op == T_OP_ADD) {
            hw(c, 0x1800u | (unsigned)(rm << 6) | (unsigned)(rn << 3) |
                  (unsigned)rd);
            return;
        }
        if (op == T_OP_SUB) {
            hw(c, 0x1a00u | (unsigned)(rm << 6) | (unsigned)(rn << 3) |
                  (unsigned)rd);
            return;
        }
        /* The rest are two-operand: only when the destination IS the
         * left operand, which is what the encoding can say. */
        if (rd == rn) {
            int n = narrow_dp(op);
            if (n >= 0) {
                hw(c, 0x4000u | (unsigned)(n << 6) | (unsigned)(rm << 3) |
                      (unsigned)rd);
                return;
            }
        }
    }
    hw2(c, 0xea00u | (unsigned)(op << 5) | (unsigned)(s << 4) | (unsigned)rn,
           (unsigned)(rd << 8) | (unsigned)rm);
}

void t_alu_reg_shift(struct code *c, int op, int rd, int rn, int rm,
                     int type, int amount, int s)
{
    A32(a32_alu_reg_shift(c, op, rd, rn, rm, type, amount, s));
    /* The same first halfword as t_alu_reg's 32-bit form; the amount is
     * split imm3:imm2 around rd, and the type sits below it. */
    unsigned imm3 = (unsigned)(amount >> 2) & 7, imm2 = (unsigned)amount & 3;
    hw2(c, 0xea00u | (unsigned)(op << 5) | (unsigned)(s << 4) | (unsigned)rn,
           (imm3 << 12) | (unsigned)(rd << 8) | (imm2 << 6) |
           (unsigned)(type << 4) | (unsigned)rm);
}

void t_bfx(struct code *c, int rd, int rn, int lsb, int width, int sign)
{
    A32(a32_bfx(c, rd, rn, lsb, width, sign));
    unsigned imm3 = (unsigned)(lsb >> 2) & 7, imm2 = (unsigned)lsb & 3;
    hw2(c, (sign ? 0xf340u : 0xf3c0u) | (unsigned)rn,
           (imm3 << 12) | (unsigned)(rd << 8) | (imm2 << 6) |
           (unsigned)(width - 1));
}

int t_alu_imm(struct code *c, int op, int rd, int rn, long imm, int s)
{
    if (t_isa_a32) return a32_alu_imm(c, op, rd, rn, imm, s);
    int e;
    if (s && low(rd) && low(rn) && imm >= 0 && imm <= 7 &&
        (op == T_OP_ADD || op == T_OP_SUB)) {
        hw(c, (op == T_OP_ADD ? 0x1c00u : 0x1e00u) |
              (unsigned)(imm << 6) | (unsigned)(rn << 3) | (unsigned)rd);
        return 1;
    }
    if (s && low(rd) && rd == rn && imm >= 0 && imm <= 255 &&
        (op == T_OP_ADD || op == T_OP_SUB)) {
        hw(c, (op == T_OP_ADD ? 0x3000u : 0x3800u) |
              (unsigned)(rd << 8) | (unsigned)imm);
        return 1;
    }
    e = encode_imm((unsigned long)imm);
    if (e < 0)
        return 0;
    hw2(c, 0xf000u | (imm_i(e) << 10) | (unsigned)(op << 5) |
           (unsigned)(s << 4) | (unsigned)rn,
           (imm_hi3(e) << 12) | (unsigned)(rd << 8) | imm_lo8(e));
    return 1;
}

static void addsubw(struct code *c, int rd, int rn, long imm, int sub)
{
    unsigned v = (unsigned)imm & 0xfff;
    hw2(c, (sub ? 0xf2a0u : 0xf200u) | (((v >> 11) & 1) << 10) | (unsigned)rn,
           (((v >> 8) & 7) << 12) | (unsigned)(rd << 8) | (v & 0xff));
}

void t_addw(struct code *c, int rd, int rn, long imm)
{
    A32(a32_addsubw(c, rd, rn, imm, 0));
    addsubw(c, rd, rn, imm, 0);
}
void t_subw(struct code *c, int rd, int rn, long imm)
{
    A32(a32_addsubw(c, rd, rn, imm, 1));
    addsubw(c, rd, rn, imm, 1);
}

void t_shift_imm(struct code *c, int op, int rd, int rm, int sh, int s)
{
    A32(a32_shift_imm(c, op, rd, rm, sh, s));
    if (op == T_SH_LSL && sh == 0) {
        if (s && low(rd) && low(rm))
            hw(c, 0x0000u | (unsigned)(rm << 3) | (unsigned)rd);  /* movs */
        else
            t_mov_reg(c, rd, rm);
        return;
    }
    if (s && low(rd) && low(rm) && op != T_SH_ROR) {
        /* LSR #32 and ASR #32 are encoded as 0 in the five-bit field. */
        unsigned f = (unsigned)(sh & 31);
        hw(c, (unsigned)(op << 11) | (f << 6) | (unsigned)(rm << 3) |
              (unsigned)rd);
        return;
    }
    hw2(c, 0xea4fu | (unsigned)(s << 4),
           (unsigned)(((sh >> 2) & 7) << 12) | (unsigned)(rd << 8) |
           (unsigned)((sh & 3) << 6) | (unsigned)(op << 4) | (unsigned)rm);
}

void t_shift_reg(struct code *c, int op, int rd, int rn, int rm, int s)
{
    A32(a32_shift_reg(c, op, rd, rn, rm, s));
    if (s && low(rd) && low(rm) && rd == rn && op != T_SH_ROR) {
        static const int n16[3] = { 2, 3, 4 };  /* LSLS, LSRS, ASRS */
        hw(c, 0x4000u | (unsigned)(n16[op] << 6) | (unsigned)(rm << 3) |
              (unsigned)rd);
        return;
    }
    hw2(c, 0xfa00u | (unsigned)(op << 5) | (unsigned)(s << 4) | (unsigned)rn,
           0xf000u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_mul(struct code *c, int rd, int rn, int rm)
{
    A32(a32_mul(c, rd, rn, rm));
    if (low(rd) && low(rn) && rd == rm) {
        hw(c, 0x4340u | (unsigned)(rn << 3) | (unsigned)rd);   /* muls */
        return;
    }
    hw2(c, 0xfb00u | (unsigned)rn, 0xf000u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_mla(struct code *c, int rd, int rn, int rm, int ra)
{
    A32(a32_mla(c, rd, rn, rm, ra, 0));
    hw2(c, 0xfb00u | (unsigned)rn,
           (unsigned)(ra << 12) | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_mls(struct code *c, int rd, int rn, int rm, int ra)
{
    A32(a32_mla(c, rd, rn, rm, ra, 1));
    hw2(c, 0xfb00u | (unsigned)rn,
           (unsigned)(ra << 12) | (unsigned)(rd << 8) | 0x10u | (unsigned)rm);
}

void t_div(struct code *c, int rd, int rn, int rm, int sign)
{
    A32(a32_div(c, rd, rn, rm, sign));
    hw2(c, (sign ? 0xfb90u : 0xfbb0u) | (unsigned)rn,
           0xf0f0u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_mull(struct code *c, int rdlo, int rdhi, int rn, int rm, int sign)
{
    A32(a32_mull(c, rdlo, rdhi, rn, rm, sign));
    hw2(c, (sign ? 0xfb80u : 0xfba0u) | (unsigned)rn,
           (unsigned)(rdlo << 12) | (unsigned)(rdhi << 8) | (unsigned)rm);
}

void t_cmp_reg(struct code *c, int rn, int rm)
{
    A32(a32_cmp_reg(c, rn, rm));
    if (low(rn) && low(rm)) {
        hw(c, 0x4280u | (unsigned)(rm << 3) | (unsigned)rn);
        return;
    }
    /* CMP (register) T2 reaches the high registers through its N bit. */
    hw(c, 0x4500u | (unsigned)((rn & 8) << 4) | (unsigned)(rm << 3) |
          (unsigned)(rn & 7));
}

void t_cmp_imm(struct code *c, int rn, long imm)
{
    A32(a32_cmp_imm(c, rn, imm));
    int e;
    if (low(rn) && imm >= 0 && imm <= 255) {
        hw(c, 0x2800u | (unsigned)(rn << 8) | (unsigned)imm);
        return;
    }
    e = encode_imm((unsigned long)imm);
    /* CMP is SUB with S set and rd = PC; the caller checked t_imm_ok. */
    hw2(c, 0xf1b0u | (imm_i(e) << 10) | (unsigned)rn,
           (imm_hi3(e) << 12) | 0x0f00u | imm_lo8(e));
}

void t_tst_reg(struct code *c, int rn, int rm)
{
    A32(a32_tst_reg(c, rn, rm));
    if (low(rn) && low(rm)) {
        hw(c, 0x4200u | (unsigned)(rm << 3) | (unsigned)rn);
        return;
    }
    hw2(c, 0xea10u | (unsigned)rn, 0x0f00u | (unsigned)rm);
}

void t_ext(struct code *c, int rd, int rm, int size, int sign)
{
    A32(a32_ext(c, rd, rm, size, sign));
    unsigned base16 = size == 1 ? (sign ? 0xb240u : 0xb2c0u)
                                : (sign ? 0xb200u : 0xb280u);
    if (low(rd) && low(rm)) {
        hw(c, base16 | (unsigned)(rm << 3) | (unsigned)rd);
        return;
    }
    hw2(c, size == 1 ? (sign ? 0xfa4fu : 0xfa5fu)
                     : (sign ? 0xfa0fu : 0xfa1fu),
           0xf080u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_clz(struct code *c, int rd, int rm)
{
    A32(a32_bitop(c, A32_CLZ, rd, rm));
    hw2(c, 0xfab0u | (unsigned)rm, 0xf080u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_rev(struct code *c, int rd, int rm)
{
    A32(a32_bitop(c, A32_REV, rd, rm));
    if (low(rd) && low(rm)) {
        hw(c, 0xba00u | (unsigned)(rm << 3) | (unsigned)rd);
        return;
    }
    hw2(c, 0xfa90u | (unsigned)rm, 0xf080u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_rev16(struct code *c, int rd, int rm)
{
    A32(a32_bitop(c, A32_REV16, rd, rm));
    if (low(rd) && low(rm)) {
        hw(c, 0xba40u | (unsigned)(rm << 3) | (unsigned)rd);
        return;
    }
    hw2(c, 0xfa90u | (unsigned)rm, 0xf090u | (unsigned)(rd << 8) | (unsigned)rm);
}

/* ---- memory --------------------------------------------------------- */

/* The 16-bit register-offset block, 0101 opc rm rn rt, indexed by
 * (store, size, sign). */
static unsigned narrow_ldst_reg(int size, int sign, int store)
{
    if (store)
        return size == 1 ? 0x5400u : size == 2 ? 0x5200u : 0x5000u;
    if (size == 1) return sign ? 0x5600u : 0x5c00u;
    if (size == 2) return sign ? 0x5e00u : 0x5a00u;
    return 0x5800u;
}

/* The 32-bit load/store's first halfword, before rn. This is the
 * imm12 form; the negative-offset and register-offset forms are the
 * same opcode with bit 7 CLEAR (0xf8d0 ldr.w [rn,#imm12] against
 * 0xf850 for [rn,#-imm8] and [rn,rm,lsl #n]), which is what
 * WIDE_ALT_MASK takes off below. */
#define WIDE_ALT_MASK 0x0080u

static unsigned wide_ldst_op(int size, int sign, int store)
{
    if (store)
        return size == 1 ? 0xf880u : size == 2 ? 0xf8a0u : 0xf8c0u;
    if (size == 1) return sign ? 0xf990u : 0xf890u;
    if (size == 2) return sign ? 0xf9b0u : 0xf8b0u;
    return 0xf8d0u;
}

int t_ldst_pair(struct code *c, int rt, int rt2, int rn, long off, int store)
{
    if (t_isa_a32) return a32_ldst_pair(c, rt, rt2, rn, off, store);
    /* LDRD/STRD (immediate) T1, offset addressing: 1110 100 P U 1 W L Rn,
     * then Rt Rt2 imm8, with P = 1 and W = 0; imm8 counts words. */
    if (rt >= T_SP || rt2 >= T_SP || rn == T_PC || (!store && rt == rt2))
        return 0;
    if (off % 4 != 0 || off < -1020 || off > 1020)
        return 0;
    unsigned u = off >= 0 ? 1u : 0u;
    unsigned mag = (unsigned)(off >= 0 ? off : -off);
    hw2(c, 0xe940u | (u << 7) | (store ? 0u : 0x10u) | (unsigned)rn,
           (unsigned)(rt << 12) | (unsigned)(rt2 << 8) | (mag / 4));
    return 1;
}

int t_ldst_imm(struct code *c, int rt, int rn, long off, int size, int sign,
               int store)
{
    if (t_isa_a32) return a32_ldst_imm(c, rt, rn, off, size, sign, store);
    /* Signedness means something only for a load narrower than the
     * register. A four-byte `load.4:4s` or any store carrying it was sent
     * to the 32-bit forms, and every signed int read through a pointer
     * cost two bytes more than it had to. */
    if (store || size >= 4)
        sign = 0;
    /* The scaled 16-bit forms: five bits times the access size, low
     * registers only, and no sign-extending member. */
    if (low(rt) && low(rn) && !sign && off >= 0 && (off % size) == 0 &&
        (off / size) <= 31) {
        unsigned f = (unsigned)(off / size);
        unsigned base = size == 1 ? (store ? 0x7000u : 0x7800u)
                      : size == 2 ? (store ? 0x8000u : 0x8800u)
                                  : (store ? 0x6000u : 0x6800u);
        hw(c, base | (f << 6) | (unsigned)(rn << 3) | (unsigned)rt);
        return 1;
    }
    /* [sp, #imm8*4], which is most of what a frame reference is. */
    if (low(rt) && rn == T_SP && size == 4 && !sign && off >= 0 &&
        (off % 4) == 0 && (off / 4) <= 255) {
        hw(c, (store ? 0x9000u : 0x9800u) | (unsigned)(rt << 8) |
              (unsigned)(off / 4));
        return 1;
    }
    if (off >= 0 && off <= 4095) {
        hw2(c, wide_ldst_op(size, sign, store) | (unsigned)rn,
               (unsigned)(rt << 12) | (unsigned)off);
        return 1;
    }
    /* The T4 form: a signed eight-bit offset, which is the only way to
     * reach backwards from a register. Its second halfword carries P, U
     * and W beside the offset — 1, U, 0 for plain offset addressing with
     * no writeback, so the constant part is 0xc00 and not 0x800. Leaving
     * P clear encodes an UNPRIVILEGED access, which is a different
     * instruction that disassembles as <unknown> on this profile. */
    if (off >= -255 && off <= 255) {
        unsigned u = off >= 0 ? 1u : 0u;
        unsigned mag = (unsigned)(off >= 0 ? off : -off);
        hw2(c, (wide_ldst_op(size, sign, store) & ~WIDE_ALT_MASK) | (unsigned)rn,
               (unsigned)(rt << 12) | 0x0c00u | (u << 9) | mag);
        return 1;
    }
    return 0;
}

/* The T4 form with writeback: W set, and P saying whether the offset
 * is added before the access ([rn, #off]!) or after it ([rn], #off,
 * P clear -- which with W clear would be the unprivileged access, see
 * above). A load or store that writes back into its own data register
 * is UNPREDICTABLE, and so refused. */
int t_ldst_wb(struct code *c, int rt, int rn, long off, int size, int sign,
              int store, int pre)
{
    if (t_isa_a32) return a32_ldst_wb(c, rt, rn, off, size, sign, store, pre);
    if (store || size >= 4)
        sign = 0;
    if (rn == T_PC || rn == rt || off < -255 || off > 255)
        return 0;
    unsigned u = off >= 0 ? 1u : 0u;
    unsigned mag = (unsigned)(off >= 0 ? off : -off);
    hw2(c, (wide_ldst_op(size, sign, store) & ~WIDE_ALT_MASK) | (unsigned)rn,
           (unsigned)(rt << 12) | 0x0900u | (pre ? 0x0400u : 0u) | (u << 9) |
           mag);
    return 1;
}

int t_ldst_reg_ok(int shift, int size, int sign, int store)
{
    return !t_isa_a32 || a32_ldst_reg_ok(shift, size, sign, store);
}

void t_ldst_reg(struct code *c, int rt, int rn, int rm, int shift, int size,
                int sign, int store)
{
    A32(a32_ldst_reg(c, rt, rn, rm, shift, size, sign, store));
    if (shift == 0 && low(rt) && low(rn) && low(rm)) {
        hw(c, narrow_ldst_reg(size, sign, store) | (unsigned)(rm << 6) |
              (unsigned)(rn << 3) | (unsigned)rt);
        return;
    }
    hw2(c, (wide_ldst_op(size, sign, store) & ~WIDE_ALT_MASK) | (unsigned)rn,
           (unsigned)(rt << 12) | (unsigned)(shift << 4) | (unsigned)rm);
}

void t_add_sp(struct code *c, int rd, long off)
{
    A32(a32_add_sp(c, rd, off));
    if (low(rd) && off >= 0 && (off % 4) == 0 && (off / 4) <= 255) {
        hw(c, 0xa800u | (unsigned)(rd << 8) | (unsigned)(off / 4));
        return;
    }
    if (off >= 0 && off <= 4095) {
        t_addw(c, rd, T_SP, off);
        return;
    }
    t_mov_imm(c, rd, off, 0);
    t_alu_reg(c, T_OP_ADD, rd, T_SP, rd, 0);
}

void t_sp_adjust(struct code *c, long imm, int sub)
{
    A32(a32_sp_adjust(c, imm, sub));
    if (imm >= 0 && (imm % 4) == 0 && (imm / 4) <= 127) {
        hw(c, (sub ? 0xb080u : 0xb000u) | (unsigned)(imm / 4));
        return;
    }
    if (imm >= 0 && imm <= 4095) {
        if (sub) t_subw(c, T_SP, T_SP, imm);
        else     t_addw(c, T_SP, T_SP, imm);
        return;
    }
    t_mov_imm(c, T_ACC, imm, 0);
    t_alu_reg(c, sub ? T_OP_SUB : T_OP_ADD, T_SP, T_SP, T_ACC, 0);
}

/* push/pop: the 16-bit form (T1) when the list is r0-r7 plus lr (push)
 * or pc (pop), which is every function whose saves are all low -- and a
 * leaf's `push {r3, lr}` / `pop {r3, pc}` is two bytes each. */
int t_push(struct code *c, unsigned mask)
{
    if (t_isa_a32) return a32_push(c, mask);
    int at = c->len;
    if (!(mask & ~(0xffu | (1u << 14))))
        hw(c, 0xb400u | ((mask >> 14) & 1u) << 8 | (mask & 0xffu));
    else
        hw2(c, 0xe92du, mask & 0x5fffu);
    return at;
}

int t_pop(struct code *c, unsigned mask)
{
    if (t_isa_a32) return a32_pop(c, mask);
    int at = c->len;
    if (!(mask & ~(0xffu | (1u << 15))))
        hw(c, 0xbc00u | ((mask >> 15) & 1u) << 8 | (mask & 0xffu));
    else
        hw2(c, 0xe8bdu, mask & 0xdfffu);
    return at;
}

/* The mask is re-written in whichever form t_push chose; it cannot
 * change form, because the size would move everything after it. */
void t_patch_push(struct code *c, int at, unsigned mask)
{
    A32(a32_patch_mask(c, at, mask & 0x5fffu));
    if (((unsigned)(c->p[at + 1] << 8 | c->p[at]) & 0xfe00u) == 0xb400u) {
        if (mask & ~(0xffu | (1u << 14)))
        {
            /* emit.c is linked into the encoding checkers too, which
             * carry no driver: no internal_error here. */
            fprintf(stderr, "embcc: internal: thumb: a 16-bit push "
                            "patched with a mask it cannot encode\n");
            abort();
        }
        patch_hw(c, at, 0xb400u | ((mask >> 14) & 1u) << 8 | (mask & 0xffu));
        return;
    }
    patch_hw(c, at + 2, mask & 0x5fffu);
}

void t_patch_pop(struct code *c, int at, unsigned mask)
{
    A32(a32_patch_mask(c, at, mask & 0xdfffu));
    patch_hw(c, at + 2, mask & 0xdfffu);
}

/* ---- control flow ---------------------------------------------------- */

int t_b(struct code *c)
{
    int at = c->len;
    if (t_isa_a32)
        return a32_b(c, -1, 0);
    hw2(c, 0xf000u, 0x9000u);
    return at;
}

int t_adr_w(struct code *c, int rd, int imm12)
{
    if (t_isa_a32) { int at = c->len; if (!a32_adr(c, rd, imm12)) a32_refuse("adr", imm12); return at; }
    /* 11110 i 1 0 0 0 0 0 1111 | 0 imm3 Rd imm8. llvm-mc: adr.w r11, .+100
     * from a 4-aligned pc is f20f 0b60. */
    int at = c->len;
    unsigned i1 = (unsigned)(imm12 >> 11) & 1, imm3 = (unsigned)(imm12 >> 8) & 7;
    unsigned imm8 = (unsigned)imm12 & 0xff;
    hw2(c, 0xf20fu | (i1 << 10), (imm3 << 12) | ((unsigned)rd << 8) | imm8);
    return at;
}

int t_tbh(struct code *c, int rm)
{
    if (t_isa_a32) a32_refuse("tbh", rm);
    /* 1110 1000 1101 1111 | 1111 0000 0001 Rm. llvm-mc: tbh [pc, r0,
     * lsl #1] = e8df f010, [pc, r12, lsl #1] = e8df f01c. */
    int at = c->len;
    hw2(c, 0xe8dfu, 0xf010u | (unsigned)rm);
    return at;
}

void t_patch_hw16(struct code *c, int at, unsigned v)
{
    c->p[at] = (unsigned char)(v & 0xff);
    c->p[at + 1] = (unsigned char)((v >> 8) & 0xff);
}

void t_patch_adr_w(struct code *c, int at, int rd, int imm12)
{
    if (t_isa_a32) { if (!a32_patch_adr(c, at, rd, imm12)) a32_refuse("adr", imm12); return; }
    unsigned i1 = (unsigned)(imm12 >> 11) & 1, imm3 = (unsigned)(imm12 >> 8) & 7;
    unsigned imm8 = (unsigned)imm12 & 0xff;
    unsigned h1 = 0xf20fu | (i1 << 10);
    unsigned h2 = (imm3 << 12) | ((unsigned)rd << 8) | imm8;
    code_patch32(c, at, (unsigned long)h1 | ((unsigned long)h2 << 16));
}
int t_bl(struct code *c)
{
    int at = c->len;
    if (t_isa_a32)
        return a32_b(c, -1, 1);
    hw2(c, 0xf000u, 0xd000u);
    return at;
}

int t_bcond(struct code *c, int cond)
{
    if (t_isa_a32) return a32_b(c, cond, 0);
    int at = c->len;
    hw2(c, 0xf000u | (unsigned)(cond << 6), 0x8000u);
    return at;
}

/* The ±16MB form, shared by b.w and bl: S:I1:I2:imm10:imm11, where I1 and
 * I2 are stored as J1 = NOT(I1 XOR S) and J2 = NOT(I2 XOR S). That
 * double negation is the trap in this encoding — it exists so a short
 * forward branch has J1 = J2 = 1 and looks like the older ARM form. */
/* A branch offset past its form's reach: refused, since an offset that
 * does not fit would encode a jump somewhere else. emit.c is linked into
 * the encoding checkers too, which carry no driver: no internal_error. */
static void out_of_reach(const char *what, long off)
{
    fprintf(stderr, "embcc: internal: thumb: %s cannot reach %ld bytes\n",
            what, off);
    abort();
}

static void patch_b24(struct code *c, int at, int target, unsigned keep)
{
    long off = (long)target - (long)at - 4;
    unsigned long v = (unsigned long)off >> 1;
    /* +-16 MB. An offset past it would encode a jump somewhere else. */
    if (off < -16777216L || off > 16777214L || (off & 1))
        out_of_reach("a 32-bit branch", off);
    unsigned s = (unsigned)((v >> 23) & 1);
    unsigned i1 = (unsigned)((v >> 22) & 1), i2 = (unsigned)((v >> 21) & 1);
    unsigned j1 = (~(i1 ^ s)) & 1, j2 = (~(i2 ^ s)) & 1;
    patch_hw(c, at, 0xf000u | (s << 10) | (unsigned)((v >> 11) & 0x3ff));
    patch_hw(c, at + 2, keep | (j1 << 13) | (j2 << 11) |
                        (unsigned)(v & 0x7ff));
}

void t_patch_b(struct code *c, int at, int target)
{
    if (t_isa_a32) {
        if (!a32_patch_b(c, at, target))
            out_of_reach("an ARM branch", (long)target - at - 8);
        return;
    }
    patch_b24(c, at, target, 0x9000u);
}
void t_patch_bl(struct code *c, int at, int target)
{
    if (t_isa_a32) {
        if (!a32_patch_b(c, at, target))
            out_of_reach("an ARM call", (long)target - at - 8);
        return;
    }
    patch_b24(c, at, target, 0xd000u);
}

/* The ±1MB conditional form: S:J2:J1:imm6:imm11, with J1 and J2 stored
 * straight rather than through the exclusive-or above. */
void t_patch_bcond(struct code *c, int at, int target)
{
    if (t_isa_a32) { if (!a32_patch_b(c, at, target)) out_of_reach("an ARM branch", (long)target - at - 8); return; }
    long off = (long)target - (long)at - 4;
    unsigned long v = (unsigned long)off >> 1;
    /* +-1 MB. This had no check: a conditional branch further away -- a
     * 1.7 MB function at -O0 -- was encoded with its offset's top bits
     * dropped, a branch to somewhere else that assembled cleanly. The
     * code generator relaxes such branches; this refuses any it missed. */
    if (off < -1048576L || off > 1048574L || (off & 1))
        out_of_reach("a conditional branch", off);
    unsigned cond = (unsigned)((c->p[at + 1] << 8 | c->p[at]) >> 6) & 0xf;
    unsigned s = (unsigned)((v >> 19) & 1);
    unsigned j2 = (unsigned)((v >> 18) & 1), j1 = (unsigned)((v >> 17) & 1);
    patch_hw(c, at, 0xf000u | (s << 10) | (cond << 6) |
                    (unsigned)((v >> 11) & 0x3f));
    patch_hw(c, at + 2, 0x8000u | (j1 << 13) | (j2 << 11) |
                        (unsigned)(v & 0x7ff));
}

/* The 16-bit branches: B<c> (T1) reaches -256..+254 bytes and B (T2)
 * -2048..+2046, both from the instruction's address plus four -- the
 * same base as the wide forms. Emitted with a zero offset and patched
 * once the target is placed; the patch refuses an offset that does not
 * fit rather than wrapping it into a jump somewhere else. */
int t_bcond16(struct code *c, int cond)
{
    if (t_isa_a32) return a32_b(c, cond, 0);
    int at = c->len;
    hw(c, 0xD000u | (unsigned)(cond << 8));
    return at;
}
int t_b16(struct code *c)
{
    int at = c->len;
    if (t_isa_a32)
        return a32_b(c, -1, 0);
    hw(c, 0xE000u);
    return at;
}

/* cbz/cbnz Rn, label: 1011 o0i1 iiii irrr, a FORWARD branch of 0..126
 * bytes from pc+4 on r0-r7 being zero (o=0) or not (o=1). No flags. */
int t_cbz(struct code *c, int nonzero, int rn)
{
    if (t_isa_a32) a32_refuse("cbz", rn);
    int at = c->len;
    if (rn < 0 || rn > 7) {
        /* emit.c is linked into the encoding checkers, which carry no
         * driver: no internal_error here. */
        fprintf(stderr, "embcc: internal: thumb: cbz on r%d, which is not "
                        "a low register\n", rn);
        abort();
    }
    hw(c, 0xB100u | (unsigned)(nonzero ? 0x800 : 0) | (unsigned)rn);
    return at;
}
int t_patch_cbz(struct code *c, int at, int target)
{
    if (t_isa_a32) a32_refuse("cbz", at);
    long off = (long)target - (long)at - 4;
    unsigned h = (unsigned)(c->p[at + 1] << 8 | c->p[at]);
    if (off < 0 || off > 126 || (off & 1))
        return 0;
    patch_hw(c, at, (h & 0xfd07u) | (unsigned)(((off >> 6) & 1) << 9) |
                    (unsigned)(((off >> 1) & 0x1f) << 3));
    return 1;
}

int t_patch_bcond16(struct code *c, int at, int target)
{
    if (t_isa_a32) return a32_patch_b(c, at, target);
    long off = (long)target - (long)at - 4;
    unsigned h = (unsigned)(c->p[at + 1] << 8 | c->p[at]);
    if (off < -256 || off > 254 || (off & 1))
        return 0;
    patch_hw(c, at, (h & 0xff00u) | (unsigned)((off >> 1) & 0xff));
    return 1;
}
int t_patch_b16(struct code *c, int at, int target)
{
    if (t_isa_a32) return a32_patch_b(c, at, target);
    long off = (long)target - (long)at - 4;
    if (off < -2048 || off > 2046 || (off & 1))
        return 0;
    patch_hw(c, at, 0xE000u | (unsigned)((off >> 1) & 0x7ff));
    return 1;
}

/* The byte and halfword exclusives and ARMv8-M's load-acquire and
 * store-release forms are one group, T1 of each:
 *
 *   1110 1000 110 L Rn | Rt 1111 op Rd
 *
 * L 1 for a load; op 0100/0101 the byte/halfword exclusive, 1000/1001/1010
 * the plain acquire/release at one, two and four bytes, 1100/1101/1110 the
 * exclusive acquire/release at the same three. Rd is the store-exclusive's
 * status register, and 1111 in every form that has none. */
static void ldst_xa(struct code *c, int load, unsigned op, int rt, int rn,
                    int rd)
{
    hw2(c, 0xe8c0u | (load ? 0x10u : 0u) | (unsigned)rn,
           ((unsigned)rt << 12) | 0x0f00u | (op << 4) |
           (rd < 0 ? 0xfu : (unsigned)rd));
}
/* `op` for a size (1, 2 or 4) in one of the three sets: 0 the exclusive
 * (byte and halfword only: the word form is t_ldrex), 1 acquire/release,
 * 2 exclusive acquire/release. */
static unsigned xa_op(int set, int size)
{
    unsigned base = set == 0 ? 4u : set == 1 ? 8u : 12u;
    return base + (size == 1 ? 0u : size == 2 ? 1u : 2u);
}

/* The byte and halfword exclusives (ARMv7-M and v8-M have both):
 * ldrexb/ldrexh zero-extend, and strexb/strexh put 0 in rd on success.
 * No offset form exists for these. `size` is 1 or 2. */
void t_ldrexbh(struct code *c, int rt, int rn, int size)
{
    A32(a32_ldrex(c, rt, rn, size));
    ldst_xa(c, 1, xa_op(0, size), rt, rn, -1);
}
void t_strexbh(struct code *c, int rd, int rt, int rn, int size)
{
    A32(a32_strex(c, rd, rt, rn, size));
    ldst_xa(c, 0, xa_op(0, size), rt, rn, rd);
}

/* ARMv8-M's load-acquire (lda, ldab, ldah; with `ex`, ldaex, ldaexb,
 * ldaexh), store-release (stl, stlb, stlh) and store-release exclusive
 * (stlex, stlexb, stlexh). Both profiles, Baseline included. */
void t_lda(struct code *c, int rt, int rn, int size, int ex)
{
    if (t_isa_a32) a32_refuse("an ARMv8-M load-acquire", rt);
    ldst_xa(c, 1, xa_op(ex ? 2 : 1, size), rt, rn, -1);
}
void t_stl(struct code *c, int rt, int rn, int size)
{
    if (t_isa_a32) a32_refuse("an ARMv8-M store-release", rt);
    ldst_xa(c, 0, xa_op(1, size), rt, rn, -1);
}
void t_stlex(struct code *c, int rd, int rt, int rn, int size)
{
    if (t_isa_a32) a32_refuse("an ARMv8-M store-release", rt);
    ldst_xa(c, 0, xa_op(2, size), rt, rn, rd);
}

/* ---- the security extension (TrustZone-M) --------------------------------
 *
 * TT Rd, Rn: 1110 1000 0100 Rn | 1111 Rd A T 000000 -- the strex word
 * form's opcode with Rt 1111 and no offset. A asks about the other
 * security state (tta, ttat; Secure only), T about unprivileged access
 * (ttt, ttat). */
void t_tt(struct code *c, int rd, int rn, int alt, int unpriv)
{
    if (t_isa_a32) a32_refuse("tt", rd);
    hw2(c, 0xe840u | (unsigned)rn,
           0xf000u | ((unsigned)rd << 8) | (alt ? 0x80u : 0u) |
           (unpriv ? 0x40u : 0u));
}

/* SG: the secure gateway, the first instruction of every entry a
 * Non-secure caller may branch to. Its two halfwords are the same,
 * 1110 1001 0111 1111, so it cannot be entered half way. */
void t_sg(struct code *c)
{
    if (t_isa_a32) a32_refuse("sg", 0);
    hw2(c, 0xe97fu, 0xe97fu);
}

/* BXNS / BLXNS Rm: bx and blx with bit 2 set -- the branch that may leave
 * the Secure state (when Rm's bit 0 is clear). */
void t_bxns(struct code *c, int rm, int link)
{
    if (t_isa_a32) a32_refuse("bxns", rm);
    hw(c, (link ? 0x4780u : 0x4700u) | (unsigned)(rm << 3) | 4u);
}

/* VLSTM / VLLDM Rn: 1110 1100 001 L Rn | 0000 1010 0000 0000. The lazy
 * save and restore of the floating-point state around a call to the
 * Non-secure state (ARMv8-M Mainline; a no-op on a part with no FPU). */
void t_vlstm(struct code *c, int rn, int load)
{
    if (t_isa_a32) a32_refuse("vlstm", rn);
    hw2(c, 0xec20u | (load ? 0x10u : 0u) | (unsigned)rn, 0x0a00u);
}

/* Is the 32-bit Thumb instruction h:h2 one an ARMv6-M core has (BL, MRS,
 * MSR, DMB, DSB, ISB), or with `v8b` one ARMv8-M Baseline has -- those
 * and the divides, the exclusives and the acquire/release forms,
 * MOVW/MOVT, B.W, CLREX, TT and SG? Matched on each one's fixed bits.
 * The backend's scan (v6m.c v6_scan) and the assembler (asm.c) both ask. */
int t_thumb1_ok32(unsigned h, unsigned h2, int v8b)
{
    unsigned op = (h2 >> 4) & 0xfu;
    if ((h & 0xf800u) == 0xf000u && (h2 & 0xd000u) == 0xd000u)
        return 1;                                    /* BL */
    if (h == 0xf3efu && (h2 & 0xf000u) == 0x8000u)
        return 1;                                    /* MRS */
    if ((h & 0xfff0u) == 0xf380u && (h2 & 0xff00u) == 0x8800u)
        return 1;                                    /* MSR */
    if (h == 0xf3bfu && (h2 & 0xff0fu) == 0x8f0fu &&
        ((h2 >> 4) & 0xf) >= 4 && ((h2 >> 4) & 0xf) <= 6)
        return 1;                                    /* DSB, DMB, ISB */
    if (!v8b)
        return 0;
    if (((h & 0xfff0u) == 0xfb90u || (h & 0xfff0u) == 0xfbb0u) &&
        (h2 & 0xf0f0u) == 0xf0f0u)
        return 1;                                    /* SDIV, UDIV */
    if ((h & 0xfff0u) == 0xe850u && (h2 & 0x0f00u) == 0x0f00u)
        return 1;                                    /* LDREX */
    if ((h & 0xfff0u) == 0xe840u)
        return 1;                          /* STREX; TT, TTT, TTA, TTAT */
    if ((h & 0xfff0u) == 0xe8d0u && (h2 & 0x0f0fu) == 0x0f0fu &&
        (op == 4 || op == 5 || op == 8 || op == 9 || op == 10 ||
         op == 12 || op == 13 || op == 14))
        return 1;     /* LDREXB/H; LDAB, LDAH, LDA; LDAEXB, LDAEXH, LDAEX */
    if ((h & 0xfff0u) == 0xe8c0u && (h2 & 0x0f00u) == 0x0f00u &&
        (op == 4 || op == 5 || op == 12 || op == 13 || op == 14 ||
         ((op == 8 || op == 9 || op == 10) && (h2 & 0xfu) == 0xfu)))
        return 1;     /* STREXB/H; STLEXB, STLEXH, STLEX; STLB, STLH, STL */
    if (h == 0xf3bfu && h2 == 0x8f2fu)
        return 1;                                    /* CLREX */
    if (h == 0xe97fu && h2 == 0xe97fu)
        return 1;                                    /* SG */
    if (((h & 0xfbf0u) == 0xf240u || (h & 0xfbf0u) == 0xf2c0u) &&
        !(h2 & 0x8000u))
        return 1;                                    /* MOVW, MOVT */
    if ((h & 0xf800u) == 0xf000u && (h2 & 0xd000u) == 0x9000u)
        return 1;                                    /* B.W */
    return 0;
}
/* clrex: drop the exclusive reservation, as a failed compare-and-swap
 * does before it leaves the loop. */
void t_clrex(struct code *c)
{
    A32(a32_clrex(c));
    hw2(c, 0xf3bfu, 0x8f2fu);
}

void t_bx(struct code *c, int rm)
{
    A32(a32_bx(c, rm, 0));
    hw(c, 0x4700u | (unsigned)(rm << 3));
}
void t_blx(struct code *c, int rm)
{
    A32(a32_bx(c, rm, 1));
    hw(c, 0x4780u | (unsigned)(rm << 3));
}
void t_nop(struct code *c)
{
    A32(a32_nop(c));
    hw(c, 0xbf00u);
}

void t_it(struct code *c, int cond, const char *te)
{
    A32(a32_it(cond, te));
    /* firstcond, then for each further instruction one mask bit: the
     * condition's own low bit for 't', its inverse for 'e' -- so the
     * instruction runs under firstcond[3:1]:bit, which is cond or its
     * inverse. Then a 1 to end the block, and zeros. */
    unsigned mask = 0;
    int k = 0;
    for (; te[k] && k < 3; k++) {
        unsigned bit = te[k] == 't' ? ((unsigned)cond & 1u)
                                    : (~(unsigned)cond & 1u);
        mask |= bit << (3 - k);
    }
    mask |= 1u << (3 - k);
    hw(c, 0xbf00u | ((unsigned)cond << 4) | mask);
}

int t_cond_invert(int cond) { return cond ^ 1; }

/* rd = cond ? 1 : 0 for a LOW register, in six bytes: `ite cond; mov rd,
 * #1; mov rd, #0`. Inside an IT block the 16-bit `mov` sets no flags,
 * which is what lets it stand where a flag-setting `movs` could not. The
 * ITE mask is the second instruction's sense -- the inverse of cond's low
 * bit, since it is the ELSE -- then the terminating 1. */
void t_setcc_low(struct code *c, int cond, int rd)
{
    A32(a32_setcc(c, cond, rd));
    hw(c, 0xbf00u | (unsigned)(cond << 4) |
          ((unsigned)(~cond & 1) << 3) | 4u);
    hw(c, 0x2000u | (unsigned)(rd << 8) | 1u);
    hw(c, 0x2000u | (unsigned)(rd << 8));
}

/* ---- the system instructions -------------------------------------------
 *
 * Each field layout is written once, here, and checked by thumbcheck.
 * They are the instructions a Cortex-M program cannot reach from C --
 * the special registers, the interrupt masks, the barriers, the hints --
 * and they exist so src/arch/thumb/asm.c has something to call rather
 * than four hex constants to copy.
 */

/* MRS <Rd>, <spec_reg>: 1111 0011 1110 1111 | 1000 Rd SYSm. */
void t_mrs(struct code *c, int rd, int sysm)
{
    if (t_isa_a32) a32_refuse("an M-profile special register", sysm);
    hw2(c, 0xF3EF, 0x8000u | ((unsigned)rd << 8) | ((unsigned)sysm & 0xff));
}

/* MSR <spec_reg>, <Rn>: 1111 0011 100 0 Rn | 1000 mask 00 SYSm, with the
 * mask 0b10 -- write the whole register, which is the only form a C
 * program wants. */
static void msr_mask(struct code *c, int sysm, int rn, unsigned mask)
{
    if (t_isa_a32) a32_refuse("an M-profile special register", sysm);
    hw2(c, 0xF380u | (unsigned)rn,
           0x8000u | (mask << 10) | ((unsigned)sysm & 0xff));
}
void t_msr(struct code *c, int sysm, int rn)
{
    msr_mask(c, sysm, rn, 2u);
}

/* MSR APSR_nzcvq, Rn -- t_msr of APSR -- or with `ge` APSR_nzcvqg, mask
 * 0b11, which writes the DSP extension's GE bits as well. */
void t_msr_apsr(struct code *c, int rn, int ge)
{
    msr_mask(c, T_SYS_APSR, rn, ge ? 3u : 2u);
}

/* CPS: 1011 0110 011 im 0 a i f. Only i and f matter on M-profile. */
void t_cps(struct code *c, int disable, int mask_i, int mask_f)
{
    A32(a32_cps(c, disable, mask_i, mask_f));
    hw(c, 0xB660u | (disable ? 0x10u : 0u) |
          (mask_i ? 2u : 0u) | (mask_f ? 1u : 0u));
}

/* DSB/DMB/ISB: 1111 0011 1011 1111 | 1000 1111 op 1111, with `op`
 * numbering them 4/5/6 and the option field 0xF ("sy", full system) --
 * the only one worth having, since a narrower barrier that is wrong is
 * indistinguishable from one that works until it does not. */
void t_barrier(struct code *c, int op)
{
    A32(a32_barrier(c, op));
    hw2(c, 0xF3BF, 0x8F0Fu | ((unsigned)op << 4));
}

/* The hints share one 16-bit encoding: 1011 1111 op 0000. */
void t_hint(struct code *c, int op)
{
    A32(a32_hint(c, op));
    hw(c, 0xBF00u | ((unsigned)op << 4));
}

void t_bkpt(struct code *c, int imm8)
{
    A32(a32_bkpt(c, imm8));
    hw(c, 0xBE00u | ((unsigned)imm8 & 0xff));
}

/* SVC: 1101 1111 imm8, B<c> T1's encoding with the condition `1111`. */
void t_svc(struct code *c, int imm8)
{
    A32(a32_svc(c, imm8));
    hw(c, 0xDF00u | ((unsigned)imm8 & 0xff));
}

int t_tst_imm(struct code *c, int rn, long imm)
{
    if (t_isa_a32) return a32_tst_imm(c, rn, imm);
    int e;
    if (!t_imm_ok(imm))
        return 0;
    e = encode_imm((unsigned long)imm);
    /* TST is AND with S set and rd = PC, as t_cmp_imm's CMP is SUB. */
    hw2(c, 0xf010u | (imm_i(e) << 10) | (unsigned)rn,
           (imm_hi3(e) << 12) | 0x0f00u | imm_lo8(e));
    return 1;
}

/* T2 STM (IA): 1110 1000 10W0 Rn | 0 M 0 list   T1 STMDB: 1110 1001 00W0 Rn
 * T2 LDM (IA): 1110 1000 10W1 Rn | P M 0 list   T1 LDMDB: 1110 1001 00W1 Rn
 * -- the same words t_push (STMDB sp!) and t_pop (LDMIA sp!) write with
 * Rn = sp and W = 1. */
int t_ldm_stm(struct code *c, int rn, unsigned mask, int wback, int before,
              int load)
{
    if (t_isa_a32) return a32_ldm_stm(c, rn, mask, wback, before, load);
    int n = 0;
    for (unsigned m = mask; m; m &= m - 1)
        n++;
    if (n < 2 || rn < 0 || rn > 14 || (mask & ~0xffffu) ||
        (mask & (1u << 13)) || (!load && (mask & (1u << 15))) ||
        (load && (mask & (1u << 15)) && (mask & (1u << 14))) ||
        (wback && (mask & (1u << rn))))
        return 0;
    hw2(c, (before ? 0xe900u : 0xe880u) | ((unsigned)wback << 5) |
           ((unsigned)load << 4) | (unsigned)rn,
        mask & 0xffffu);
    return 1;
}

static void vsplit(int r, int dbl, unsigned *field, unsigned *flag);

/* VLDM/VSTM, single precision (T2): 1110 110P UDWL Rn | Vd 1010 imm8,
 * the words t_vpush_s writes with Rn = sp. DB is P=1 U=0 and must write
 * back (P=1 W=0 is VSTR/VLDR); IA is P=0 U=1. */
int t_vldm_vstm(struct code *c, int rn, int first, int n, int wback,
                int before, int load)
{
    unsigned f, fl;
    if (n < 1 || first < 0 || first + n > 32 || rn < 0 || rn > 14 ||
        (before && !wback))
        return 0;
    vsplit(first, 0, &f, &fl);
    vhw2(c, 0xec00u | ((unsigned)before << 8) | ((unsigned)!before << 7) |
           (fl << 6) | ((unsigned)wback << 5) | ((unsigned)load << 4) |
           (unsigned)rn,
        (f << 12) | 0x0a00u | ((unsigned)n & 0xffu));
    return 1;
}

/* LDR (literal) T2: 1111 1000 U101 1111 | Rt imm12 -- the 32-bit form
 * t_ldst_imm writes for a positive offset with Rn = pc, with U for the
 * sign. */
int t_ldr_lit(struct code *c, int rt, long off)
{
    if (t_isa_a32) return a32_ldr_lit(c, rt, off);
    long mag = off < 0 ? -off : off;
    if (mag > 4095)
        return 0;
    hw2(c, (off < 0 ? 0xf85fu : 0xf8dfu),
        ((unsigned)rt << 12) | (unsigned)mag);
    return 1;
}

/* LDR (literal) T1: `ldr rt, [pc, #off]`, two bytes, for r0-r7 and a
 * FORWARD word offset of 0..1020 from Align(pc, 4). 0 when it does not
 * fit, and the caller takes the four-byte t_ldr_lit. */
int t_ldr_lit16(struct code *c, int rt, long off)
{
    if (t_isa_a32) return 0;
    if (rt < 0 || rt > 7 || off < 0 || off > 1020 || (off & 3))
        return 0;
    hw(c, 0x4800u | (unsigned)(rt << 8) | (unsigned)(off >> 2));
    return 1;
}

/* An assembly file's `ldr rd, =VALUE`, the way GNU as and LLVM's
 * assembler choose for a constant: MOV.W when it is a modified immediate,
 * MVN.W when its complement is, MOVW when it fits sixteen bits, and
 * otherwise a literal -- this returns 0 and the file assembler places
 * one. None of these sets the flags (an `ldr` does not), which is why the
 * two-byte `movs` is never one of them. sp and pc take the literal. */
int t_ldr_const(struct code *c, int rd, unsigned long v)
{
    if (t_isa_a32) return a32_ldr_const(c, rd, v);
    int e;
    v &= 0xffffffffUL;
    if (rd == 13 || rd == 15)
        return 0;
    if ((e = encode_imm(v)) >= 0) {                     /* MOV.W, S=0 */
        hw2(c, 0xf04fu | (imm_i(e) << 10),
               (imm_hi3(e) << 12) | (unsigned)(rd << 8) | imm_lo8(e));
        return 1;
    }
    if ((e = encode_imm(~v & 0xffffffffUL)) >= 0) {     /* MVN.W, S=0 */
        hw2(c, 0xf06fu | (imm_i(e) << 10),
               (imm_hi3(e) << 12) | (unsigned)(rd << 8) | imm_lo8(e));
        return 1;
    }
    if (v <= 0xffff) {
        movw(c, rd, (unsigned)v, 0);
        return 1;
    }
    return 0;
}

/* RBIT <Rd>, <Rm>: the operand appears TWICE, in both halfwords, which
 * is the encoding and not a typo. */
void t_rbit(struct code *c, int rd, int rm)
{
    A32(a32_bitop(c, A32_RBIT, rd, rm));
    hw2(c, 0xFA90u | (unsigned)rm,
           0xF0A0u | ((unsigned)rd << 8) | (unsigned)rm);
}

/* LDREX <Rt>, [<Rn>, #off] -- the offset is in WORDS in the encoding and
 * in bytes in the syntax, which is the sort of thing that is wrong by a
 * factor of four until a disassembler says so. */
void t_ldrex(struct code *c, int rt, int rn, int off)
{
    if (t_isa_a32) { if (off) a32_refuse("ldrex with an offset", off); a32_ldrex(c, rt, rn, 4); return; }
    hw2(c, 0xE850u | (unsigned)rn,
           ((unsigned)rt << 12) | 0x0F00u | (((unsigned)off >> 2) & 0xff));
}

void t_strex(struct code *c, int rd, int rt, int rn, int off)
{
    if (t_isa_a32) { if (off) a32_refuse("strex with an offset", off); a32_strex(c, rd, rt, rn, 4); return; }
    hw2(c, 0xE840u | (unsigned)rn,
           ((unsigned)rt << 12) | ((unsigned)rd << 8) |
           (((unsigned)off >> 2) & 0xff));
}

/* ---- VFP: the floating-point unit an F part has ------------------------
 *
 * Cortex-M4F is FPv4-SP-D16: SINGLE precision in hardware, sixteen
 * double registers that exist for storage and moves but have no
 * arithmetic. So `float` runs here and `double` still goes through
 * __adddf3 and friends -- and yet the hard-float ABI passes a double in
 * d0-d7 anyway, because those registers exist even where the arithmetic
 * does not. Both widths therefore have to be encodable.
 *
 * Every instruction below comes out of ONE packer, because VFP's operand
 * encoding is one rule: a register contributes four bits to a field and
 * one bit to a flag whose position says which operand it was -- D for
 * the destination, N for the first source, M for the second.
 *
 * THE TRAP, and the reason vsplit exists rather than two shifts at each
 * call site: the split is OPPOSITE for the two widths. A single s<N>
 * contributes N>>1 to the field and N&1 to the flag; a double d<N>
 * contributes N&0xf to the field and N>>4 to the flag. Encoding a double
 * the single way names a DIFFERENT REGISTER and assembles without
 * complaint -- there is no invalid encoding to catch it.
 *
 * Checked against llvm-mc instruction by instruction before any of it
 * was written here (tools/vfpcheck does it again on every test run), for
 * the same reason the RISC-V vocabulary is: a hand-written encoder with
 * no referee is a guess.
 */

/* The four-bit field and the one-bit flag a register number splits into.
 * `dbl` picks which of the two rules applies. */
static void vsplit(int r, int dbl, unsigned *field, unsigned *flag)
{
    if (dbl) {
        *field = (unsigned)r & 0xfu;
        *flag  = ((unsigned)r >> 4) & 1u;
    } else {
        *field = ((unsigned)r >> 1) & 0xfu;
        *flag  = (unsigned)r & 1u;
    }
}

/* hw1: 1110 1110 | D | group | Vn4      hw2: Vd4 | 101 | sz | N | op | M | Vm4
 *
 * `grp` carries hw1's opcode bits and `op6` hw2's, which together name
 * the operation. Nothing else varies. */
/* The destination and the source carry their OWN widths, and `sz` is a
 * third thing again -- the width of the FLOATING-POINT side.
 *
 * For everything but a conversion all three agree, which is why one flag
 * looked sufficient. A conversion is where they come apart: the integer
 * side of `vcvt` always lives in a SINGLE register, so
 * `vcvt.f64.s32 d0, s1` has a double destination and a single source,
 * and splitting both by the same rule names two wrong registers and
 * still assembles. tools/vfpcheck is what said so. */
static void vfp(struct code *c, unsigned grp, unsigned vn4, unsigned n1,
                unsigned op6, int d, int d_dbl, int m, int m_dbl, int sz)
{
    unsigned df, dfl, mf, mfl;
    vsplit(d, d_dbl, &df, &dfl);
    vsplit(m, m_dbl, &mf, &mfl);
    vhw2(c, 0xEE00u | (dfl << 6) | grp | (vn4 & 0xfu),
           (df << 12) | 0x0A00u | ((unsigned)!!sz << 8) | (n1 << 7) |
           (op6 << 6) | (mfl << 5) | mf);
}

/* d = n <op> m. The first source is a register, so it fills Vn/N. */
static void vfp_bin(struct code *c, unsigned grp, unsigned op6,
                    int d, int n, int m, int dbl)
{
    unsigned f, fl;
    vsplit(n, dbl, &f, &fl);
    vfp(c, grp, f, fl, op6, d, dbl, m, dbl, dbl);
}

/* d = <op> m. There is no first source, so Vn/N carry a five-bit
 * sub-opcode instead -- its top four bits where Vn would go and its low
 * bit where N would go, which is why vneg and vsqrt differ in one bit. */
static void vfp_un(struct code *c, unsigned opc5, int d, int m, int dbl)
{
    vfp(c, 0xB0, opc5 >> 1, opc5 & 1u, 1, d, dbl, m, dbl, dbl);
}

void t_vadd(struct code *c, int d, int n, int m, int dbl)
    { vfp_bin(c, 0x30, 0, d, n, m, dbl); }
void t_vsub(struct code *c, int d, int n, int m, int dbl)
    { vfp_bin(c, 0x30, 1, d, n, m, dbl); }
void t_vmul(struct code *c, int d, int n, int m, int dbl)
    { vfp_bin(c, 0x20, 0, d, n, m, dbl); }
void t_vdiv(struct code *c, int d, int n, int m, int dbl)
    { vfp_bin(c, 0x80, 0, d, n, m, dbl); }
/* Fused multiply-add: d += n * m, with ONE rounding. Not the same
 * value as a separate multiply and add, which is why it is only ever
 * emitted for __builtin_fma and never to fold an expression. */
void t_vfma(struct code *c, int d, int n, int m, int dbl)
    { vfp_bin(c, 0xA0, 0, d, n, m, dbl); }

void t_vmov_reg(struct code *c, int d, int m, int dbl)
    { vfp_un(c, 0, d, m, dbl); }
void t_vabs(struct code *c, int d, int m, int dbl)  { vfp_un(c, 1, d, m, dbl); }
void t_vneg(struct code *c, int d, int m, int dbl)  { vfp_un(c, 2, d, m, dbl); }
void t_vsqrt(struct code *c, int d, int m, int dbl) { vfp_un(c, 3, d, m, dbl); }
/* Sets FPSCR's flags, which only vmrs can move to APSR -- a float
 * comparison is two instructions on this machine, never one. */
void t_vcmp(struct code *c, int n, int m, int dbl) { vfp_un(c, 8, n, m, dbl); }
/* vcmpE: the same, and an Invalid Operation exception for a quiet NaN as
 * well as a signalling one -- which is what IEEE 754 asks of <, <=, >
 * and >=, and what GCC and clang emit for them. == and != use vcmp. */
void t_vcmpe(struct code *c, int n, int m, int dbl) { vfp_un(c, 9, n, m, dbl); }

/* The conversions are deliberately NOT forced into vfp_un's shape: their
 * Vn/N fields do not hold one opcode. Going TO float, N is the integer's
 * signedness; coming FROM float, Vn selects the destination's signedness
 * and N means round-toward-zero, which is the rounding C requires. */
/* Group 0xB0, the same as the unary family -- NOT 0xF0. The difference
 * is invisible whenever the destination register is odd, because the D
 * flag then sets bit 6 and 0xB0|8 and 0xF0|8 come out as the same byte.
 * Every register the first draft of this was checked against happened to
 * be odd, so it passed; tools/vfpcheck sweeps both parities and said so
 * at once. */
/* The float side's width is `dbl`; the integer side is a SINGLE
 * register either way, which is what the 0 argument says. */
void t_vcvt_f_from_i(struct code *c, int d, int m, int sgn, int dbl)
    { vfp(c, 0xB0, 8, (unsigned)!!sgn, 1, d, dbl, m, 0, dbl); }
void t_vcvt_i_from_f(struct code *c, int d, int m, int sgn, int dbl)
    { vfp(c, 0xB0, sgn ? 13u : 12u, 1, 1, d, 0, m, dbl, dbl); }
/* Between the two float widths: vcvt.f64.f32 widens (exactly) and
 * vcvt.f32.f64 narrows (rounding as FPSCR says, nearest-even at reset).
 * The same group and sub-opcode both ways; `sz` is the SOURCE's width,
 * and the destination is the other one -- the third place in this file
 * where one instruction's two registers are split by different rules. */
void t_vcvt_f_f(struct code *c, int d, int m, int to_dbl)
    { vfp(c, 0xB0, 7, 1, 1, d, to_dbl, m, !to_dbl, !to_dbl); }

/* The floating-point constants an instruction can carry: VFPExpandImm's
 * eight bits, a sign, a three-bit exponent and a four-bit fraction --
 * +-(16..31)/16 times 2^-3..2^4, so 0.5, 1.0, 2.0, 10.0 and their kin but
 * never 0.0. Found by expanding every one of the 256 and comparing, which
 * is the definition itself rather than a restatement of its bit rules.
 * -1 when `bits` (a double's pattern when `dbl`, else a float's in the
 * low 32) is not one of them. */
int t_vfp_imm8(unsigned long long bits, int dbl)
{
    for (unsigned k = 0; k < 256; k++) {
        unsigned b = (k >> 6) & 1u;
        unsigned long long sign = (unsigned long long)(k >> 7);
        unsigned long long cd = (k >> 4) & 3u, frac = k & 15u, v;
        if (dbl)
            v = sign << 63 | (unsigned long long)!b << 62 |
                (b ? 0xffULL : 0ULL) << 54 | cd << 52 | frac << 48;
        else
            v = sign << 31 | (unsigned long long)!b << 30 |
                (b ? 0x1fULL : 0ULL) << 25 | cd << 23 | frac << 19;
        if (v == bits)
            return (int)k;
    }
    return -1;
}

/* vmov.f32/.f64 <reg>, #<imm>: VFPExpandImm's byte split across the two
 * places a source register would go -- its top four bits where Vn is and
 * its low four where Vm is, with N and M zero. */
void t_vmov_imm(struct code *c, int d, int imm8, int dbl)
    { vfp(c, 0xB0, (unsigned)imm8 >> 4, 0, 0, d, dbl, imm8 & 15, 1, dbl); }

/* vldr/vstr: hw2's 0x0A00/0x0B00 selects the width exactly as it does
 * above, and the offset is in WORDS, so it reaches four times as far as
 * the byte count suggests and must be a multiple of four. */
void t_vldst(struct code *c, int sd, int rn, int off, int dbl, int store)
{
    unsigned df, dfl;
    int neg = off < 0;
    unsigned u = (unsigned)(neg ? -off : off);
    vsplit(sd, dbl, &df, &dfl);
    vhw2(c, 0xED00u | (store ? 0 : 0x10u) | ((unsigned)!neg << 7) |
           (dfl << 6) | ((unsigned)rn & 0xfu),
           (df << 12) | 0x0A00u | ((unsigned)!!dbl << 8) | ((u >> 2) & 0xffu));
}

/* A core register and a SINGLE register, either way round. `to_fp` says
 * which; the register fields do not move. */
void t_vmov_core(struct code *c, int sn, int rt, int to_fp)
{
    unsigned f, fl;
    vsplit(sn, 0, &f, &fl);
    vhw2(c, 0xEE00u | (to_fp ? 0 : 0x10u) | f,
           ((unsigned)rt << 12) | 0x0A00u | (fl << 7) | 0x10u);
}

/* A core PAIR and a double register: what the hard-float ABI needs
 * around every soft-float helper call, since the value arrives in d0 and
 * __adddf3 wants it in r0/r1. */
void t_vmov_core_pair(struct code *c, int dm, int rt, int rt2, int to_fp)
{
    unsigned f, fl;
    vsplit(dm, 1, &f, &fl);
    vhw2(c, 0xEC00u | (to_fp ? 0x40u : 0x50u) | ((unsigned)rt2 & 0xfu),
           ((unsigned)rt << 12) | 0x0B00u | (fl << 5) | 0x10u | f);
}

/* vpush / vpop of `n` consecutive registers from s`first` or d`first`:
 * the callee-saved half of the VFP file, s16-s31 or d8-d15, which a
 * function that keeps values in it must restore. The low byte counts
 * WORDS, which is the register count for singles and twice it for
 * doubles -- the .64 form saves the same memory with the D names. */
static void vpushpop(struct code *c, int first, int n, int pop, int dbl)
{
    unsigned f, fl;
    vsplit(first, dbl, &f, &fl);
    vhw2(c, (pop ? 0xECBDu : 0xED2Du) | (fl << 6),
           (f << 12) | 0x0A00u | ((unsigned)!!dbl << 8) |
           ((unsigned)(dbl ? 2 * n : n) & 0xffu));
}
void t_vpush_s(struct code *c, int first, int n, int pop)
    { vpushpop(c, first, n, pop, 0); }
void t_vpush_d(struct code *c, int first, int n, int pop)
    { vpushpop(c, first, n, pop, 1); }

/* vmrs APSR_nzcv, FPSCR -- the only way a float comparison's result
 * reaches the condition flags. */
void t_vmrs_apsr(struct code *c)
{
    vhw2(c, 0xEEF1u, 0xFA10u);
}

/* ---- ARMv6-M: the Thumb-1 forms ------------------------------------------
 *
 * ARMv6-M (Cortex-M0, M0+, M1) has the 16-bit encodings and six 32-bit
 * ones -- BL, MRS, MSR, DMB, DSB and ISB, which t_bl, t_mrs, t_msr and
 * t_barrier above already write. Every other hw2() in this file is
 * UNDEFINED there and takes a HardFault.
 *
 * The encoders above choose the 16-bit form when the operands allow it
 * and fall back to a 32-bit one when they do not: right on ARMv7-M, a
 * fault on ARMv6-M. The t1_* encoders below never widen. Each one
 *
 *  - stops with an internal error when given a register its form cannot
 *    name -- a high register where only r0-r7 fit -- because that is a
 *    bug in the caller, not a property of the operands;
 *  - returns 0 and writes nothing when an immediate or an offset does not
 *    fit its field, as t_ldst_imm and t_ldr_lit16 do, so the caller can
 *    build the value another way.
 *
 * Where an encoder above already writes this form for every operand the
 * t1_* one accepts, the t1_* one checks the operands and calls it, so
 * each encoding is written in one place. tools/t1check compares every
 * t1_* encoder, and the encoders above that ARMv6-M code reuses as they
 * are, with llvm-mc -triple=thumbv6m-none-eabi byte for byte
 * (tests/golden/thumb-v6m-encoding.sh).
 *
 * Every data-processing instruction on low registers SETS THE FLAGS here;
 * there is no other form. The only instructions that move or address
 * without touching them are MOV, ADD and CMP with a high register
 * (t_mov_reg, t1_add_hi; CMP sets them, which is its purpose), the loads
 * and stores, ADR, ADD rd, sp, #imm and the sp adjustments. MOVS changes
 * N and Z but leaves C and V alone.
 */

/* An operand a Thumb-1 form cannot name. emit.c is linked into the
 * encoding checkers too, which carry no driver: no internal_error here. */
static void t1_refuse(const char *insn, const char *what, int v)
{
    fprintf(stderr, "embcc: internal: thumb: %s with %s %d, which ARMv6-M "
                    "cannot encode\n", insn, what, v);
    abort();
}

static void t1_lo(const char *insn, int r)
{
    if (r < 0 || r > 7)
        t1_refuse(insn, "register", r);
}

static void t1_addsub_op(const char *insn, int op)
{
    if (op != T_OP_ADD && op != T_OP_SUB)
        t1_refuse(insn, "operation", op);
}

/* MOVS Rd, #imm8 (T1): 0010 0 Rd imm8. t_movs_imm's 16-bit form. */
int t1_movs_imm(struct code *c, int rd, long imm)
{
    t1_lo("movs", rd);
    if (imm < 0 || imm > 255)
        return 0;
    return t_movs_imm(c, rd, imm) == 0;
}

/* MOVS Rd, Rm (T2): 0000 0000 00 Rm Rd, which is LSLS Rd, Rm, #0.
 * t_movs_reg's 16-bit form. N and Z from Rm; C and V unchanged. */
void t1_movs_reg(struct code *c, int rd, int rm)
{
    t1_lo("movs", rd);
    t1_lo("movs", rm);
    t_movs_reg(c, rd, rm);
}

/* ADDS/SUBS Rd, Rn, Rm (T1): 0001 10 S Rm Rn Rd, S set for SUBS.
 * t_alu_reg's 16-bit three-register form. */
void t1_addsub_reg(struct code *c, int op, int rd, int rn, int rm)
{
    t1_addsub_op("adds/subs", op);
    t1_lo("adds/subs", rd);
    t1_lo("adds/subs", rn);
    t1_lo("adds/subs", rm);
    t_alu_reg(c, op, rd, rn, rm, 1);
}

/* ADDS/SUBS Rd, Rn, #imm3 (T1): 0001 11 S imm3 Rn Rd, 0..7.
 * t_alu_imm's first form. */
int t1_addsub_imm3(struct code *c, int op, int rd, int rn, long imm)
{
    t1_addsub_op("adds/subs", op);
    t1_lo("adds/subs", rd);
    t1_lo("adds/subs", rn);
    if (imm < 0 || imm > 7)
        return 0;
    return t_alu_imm(c, op, rd, rn, imm, 1);
}

/* ADDS/SUBS Rdn, #imm8 (T2): 0011 S Rdn imm8, 0..255.
 *
 * Written here, not through t_alu_imm: for 0..7 that one takes the
 * three-register imm3 form whatever Rd and Rn are. Both are two bytes and
 * compute the same thing, but they are different encodings, and this is
 * the one `adds r0, #5` assembles to. */
int t1_addsub_imm8(struct code *c, int op, int rdn, long imm)
{
    t1_addsub_op("adds/subs", op);
    t1_lo("adds/subs", rdn);
    if (imm < 0 || imm > 255)
        return 0;
    hw(c, (op == T_OP_ADD ? 0x3000u : 0x3800u) | ((unsigned)rdn << 8) |
          (unsigned)imm);
    return 1;
}

/* ANDS/EORS/ADCS/SBCS/ORRS/BICS Rdn, Rm (T1): 0100 00 op4 Rm Rdn, the
 * data-processing block. `op` is T_OP_AND, _EOR, _ADC, _SBC, _ORR or
 * _BIC; narrow_dp() above gives op4, and t_alu_reg writes the form when
 * the destination is the first operand. */
void t1_alu_reg(struct code *c, int op, int rdn, int rm)
{
    if (narrow_dp(op) < 0)
        t1_refuse("ands/eors/adcs/sbcs/orrs/bics", "operation", op);
    t1_lo("ands/eors/adcs/sbcs/orrs/bics", rdn);
    t1_lo("ands/eors/adcs/sbcs/orrs/bics", rm);
    t_alu_reg(c, op, rdn, rdn, rm, 1);
}

/* LSLS/LSRS/ASRS/RORS Rdn, Rm (T1): the data-processing block's op4 2, 3,
 * 4 and 7 -- a shift by the low byte of Rm. t_shift_reg writes the first
 * three; ROR it never needed in 16 bits, since ARMv7-M has the wide one:
 * 0100 0001 11 Rm Rdn. */
void t1_shift_reg(struct code *c, int op, int rdn, int rm)
{
    t1_lo("lsls/lsrs/asrs/rors", rdn);
    t1_lo("lsls/lsrs/asrs/rors", rm);
    if (op == T_SH_ROR) {
        hw(c, 0x41c0u | ((unsigned)rm << 3) | (unsigned)rdn);
        return;
    }
    if (op != T_SH_LSL && op != T_SH_LSR && op != T_SH_ASR)
        t1_refuse("lsls/lsrs/asrs/rors", "shift", op);
    t_shift_reg(c, op, rdn, rdn, rm, 1);
}

/* LSLS/LSRS/ASRS Rd, Rm, #sh (T1): 000 op2 imm5 Rm Rd, op2 the T_SH_*
 * number. LSL takes 0..31 (0 is MOVS Rd, Rm); LSR and ASR take 1..32,
 * with 32 written as 0. t_shift_imm's 16-bit form. Thumb-1 has no ROR by
 * an immediate. */
int t1_shift_imm(struct code *c, int op, int rd, int rm, int sh)
{
    t1_lo("lsls/lsrs/asrs", rd);
    t1_lo("lsls/lsrs/asrs", rm);
    if (op != T_SH_LSL && op != T_SH_LSR && op != T_SH_ASR)
        t1_refuse("lsls/lsrs/asrs", "shift", op);
    if (op == T_SH_LSL ? (sh < 0 || sh > 31) : (sh < 1 || sh > 32))
        return 0;
    t_shift_imm(c, op, rd, rm, sh, 1);
    return 1;
}

/* CMP Rn, Rm: T1 (0100 0010 10 Rm Rn) for two low registers, T2 (0100
 * 0101 N Rm Rn) when either is high -- both sixteen bits, which is what
 * t_cmp_reg writes. pc is UNPREDICTABLE in T2. */
void t1_cmp_reg(struct code *c, int rn, int rm)
{
    if (rn < 0 || rn > 14)
        t1_refuse("cmp", "register", rn);
    if (rm < 0 || rm > 14)
        t1_refuse("cmp", "register", rm);
    t_cmp_reg(c, rn, rm);
}

/* CMP Rn, #imm8 (T1): 0010 1 Rn imm8. t_cmp_imm's 16-bit form. */
int t1_cmp_imm(struct code *c, int rn, long imm)
{
    t1_lo("cmp", rn);
    if (imm < 0 || imm > 255)
        return 0;
    t_cmp_imm(c, rn, imm);
    return 1;
}

/* CMN Rn, Rm (T1): 0100 0010 11 Rm Rn -- the flags of Rn + Rm, which is
 * how a comparison with a small negative constant is made here. */
void t1_cmn(struct code *c, int rn, int rm)
{
    t1_lo("cmn", rn);
    t1_lo("cmn", rm);
    hw(c, 0x42c0u | ((unsigned)rm << 3) | (unsigned)rn);
}

/* TST Rn, Rm (T1): 0100 0010 00 Rm Rn. t_tst_reg's 16-bit form. */
void t1_tst(struct code *c, int rn, int rm)
{
    t1_lo("tst", rn);
    t1_lo("tst", rm);
    t_tst_reg(c, rn, rm);
}

/* RSBS Rd, Rn, #0 (T1, also spelled NEGS): 0100 0010 01 Rn Rd -- the
 * only reverse subtract ARMv6-M has, and so its only negation. */
void t1_negs(struct code *c, int rd, int rn)
{
    t1_lo("rsbs", rd);
    t1_lo("rsbs", rn);
    hw(c, 0x4240u | ((unsigned)rn << 3) | (unsigned)rd);
}

/* MVNS Rd, Rm (T1): 0100 0011 11 Rm Rd. t_mvn_reg's 16-bit form. */
void t1_mvns(struct code *c, int rd, int rm)
{
    t1_lo("mvns", rd);
    t1_lo("mvns", rm);
    t_mvn_reg(c, rd, rm, 1);
}

/* MULS Rdm, Rn, Rdm (T1): 0100 0011 01 Rn Rdm -- the low 32 bits of the
 * product, into the register that was the second factor. t_mul's 16-bit
 * form. There is no other multiply: no MLA, MLS, UMULL or SMULL. */
void t1_muls(struct code *c, int rdm, int rn)
{
    t1_lo("muls", rdm);
    t1_lo("muls", rn);
    t_mul(c, rdm, rn, rdm);
}

/* ADD Rdn, Rm (T2): 0100 0100 DN Rm Rdn -- the special data-processing
 * group that t_mov_reg (op 10, 0x4600) and t_cmp_reg's high form (op 01,
 * 0x4500) are in, with op 00. Any two registers, flags unchanged: with sp
 * as either operand it is ADD (SP plus register), the same bits. Both pc
 * is UNPREDICTABLE. */
void t1_add_hi(struct code *c, int rdn, int rm)
{
    if (rdn < 0 || rdn > 15)
        t1_refuse("add", "register", rdn);
    if (rm < 0 || rm > 15 || (rdn == 15 && rm == 15))
        t1_refuse("add", "register", rm);
    hw(c, 0x4400u | ((unsigned)(rdn & 8) << 4) | ((unsigned)rm << 3) |
          (unsigned)(rdn & 7));
}

/* SXTB/SXTH/UXTB/UXTH Rd, Rm (T1): 1011 0010 op2 Rm Rd. t_ext's 16-bit
 * form; `size` 1 or 2. */
void t1_ext(struct code *c, int rd, int rm, int size, int sign)
{
    t1_lo("sxtb/sxth/uxtb/uxth", rd);
    t1_lo("sxtb/sxth/uxtb/uxth", rm);
    if (size != 1 && size != 2)
        t1_refuse("sxtb/sxth/uxtb/uxth", "size", size);
    t_ext(c, rd, rm, size, sign);
}

/* REV / REV16 Rd, Rm (T1): 1011 1010 op2 Rm Rd, op2 00 and 01. t_rev's
 * and t_rev16's 16-bit forms. */
void t1_rev(struct code *c, int rd, int rm)
{
    t1_lo("rev", rd);
    t1_lo("rev", rm);
    t_rev(c, rd, rm);
}

void t1_rev16(struct code *c, int rd, int rm)
{
    t1_lo("rev16", rd);
    t1_lo("rev16", rm);
    t_rev16(c, rd, rm);
}

/* REVSH Rd, Rm (T1): the same group, op2 11 -- the low halfword's two
 * bytes swapped and sign-extended to 32 bits. */
void t1_revsh(struct code *c, int rd, int rm)
{
    t1_lo("revsh", rd);
    t1_lo("revsh", rm);
    hw(c, 0xbac0u | ((unsigned)rm << 3) | (unsigned)rd);
}

/* LDR/STR, LDRB/STRB, LDRH/STRH Rt, [Rn, #off] (T1): 011 B L imm5 Rn Rt
 * for a word or a byte, 1000 L imm5 Rn Rt for a halfword. The field
 * counts units of the access, so `off` is 0..31 times `size`. No
 * sign-extending form takes an immediate: LDRSB and LDRSH are register
 * offset only (t1_ldst_reg). t_ldst_imm's first form. */
int t1_ldst_imm(struct code *c, int rt, int rn, long off, int size,
                int store)
{
    t1_lo(store ? "str" : "ldr", rt);
    t1_lo(store ? "str" : "ldr", rn);
    if (size != 1 && size != 2 && size != 4)
        t1_refuse(store ? "str" : "ldr", "size", size);
    if (off < 0 || off % size != 0 || off / size > 31)
        return 0;
    return t_ldst_imm(c, rt, rn, off, size, 0, store);
}

/* LDR/STR/LDRB/STRB/LDRH/STRH/LDRSB/LDRSH Rt, [Rn, Rm] (T1): 0101 opB Rm
 * Rn Rt. t_ldst_reg's 16-bit form, through narrow_ldst_reg(). `sign`
 * matters only to a load narrower than a word. */
void t1_ldst_reg(struct code *c, int rt, int rn, int rm, int size, int sign,
                 int store)
{
    t1_lo(store ? "str" : "ldr", rt);
    t1_lo(store ? "str" : "ldr", rn);
    t1_lo(store ? "str" : "ldr", rm);
    if (size != 1 && size != 2 && size != 4)
        t1_refuse(store ? "str" : "ldr", "size", size);
    if (store || size == 4)
        sign = 0;
    t_ldst_reg(c, rt, rn, rm, 0, size, sign, store);
}

/* LDR/STR Rt, [sp, #off] (T1/T2): 1001 L Rt imm8, `off` 0..1020 in words.
 * The only sp-relative access: there is no byte, halfword or signed form
 * from sp. t_ldst_imm's sp form. */
int t1_ldst_sp(struct code *c, int rt, long off, int store)
{
    t1_lo(store ? "str" : "ldr", rt);
    if (off < 0 || off > 1020 || (off & 3))
        return 0;
    return t_ldst_imm(c, rt, T_SP, off, 4, 0, store);
}

/* ADD Rd, sp, #off (T1): 1010 1 Rd imm8, `off` 0..1020 in words -- a
 * frame address. t_add_sp's 16-bit form. Flags unchanged. */
int t1_add_sp_imm(struct code *c, int rd, long off)
{
    t1_lo("add", rd);
    if (off < 0 || off > 1020 || (off & 3))
        return 0;
    t_add_sp(c, rd, off);
    return 1;
}

/* ADD/SUB sp, sp, #imm (T2/T1): 1011 0000 S imm7, `imm` 0..508 in words.
 * t_sp_adjust's 16-bit form. A larger frame is several of these, or a
 * register: ADD sp, Rm is t1_add_hi. */
int t1_sp_adjust(struct code *c, long imm, int sub)
{
    if (imm < 0 || imm > 508 || (imm & 3))
        return 0;
    t_sp_adjust(c, imm, sub);
    return 1;
}

/* ADR Rd, #off (T1): 1010 0 Rd imm8 -- Rd = Align(pc, 4) + off, `off`
 * 0..1020 in words, FORWARD only. ARMv6-M's only pc-relative address: the
 * adr.w a jump table uses on ARMv7-M (t_adr_w) is 32-bit. */
int t1_adr(struct code *c, int rd, long off)
{
    t1_lo("adr", rd);
    if (off < 0 || off > 1020 || (off & 3))
        return 0;
    hw(c, 0xa000u | ((unsigned)rd << 8) | (unsigned)(off >> 2));
    return 1;
}

/* The word count an ADR or a literal LDR at `at` holds to reach the code
 * offset `target`: measured from Align(at + 4, 4), forward. -1 when it
 * cannot. The buffer is the section, so `at`'s alignment is the
 * instruction's own, provided the section is four-aligned. */
static long t1_pc_words(int at, int target)
{
    long off = (long)target - (((long)at + 4) & ~3L);
    if (off < 0 || off > 1020 || (off & 3))
        return -1;
    return off >> 2;
}

/* Point the instruction at `at` -- ADR (1010 0 Rd imm8) or LDR literal
 * (0100 1 Rt imm8, t_ldr_lit16) -- at `target`, keeping its register. 0
 * when the target is out of reach, which leaves the instruction as it
 * was. The literal pool and a jump table emit one with a zero offset and
 * patch it once the pool or table is placed. */
static int t1_patch_pc8(struct code *c, int at, int target, unsigned op,
                        const char *insn)
{
    unsigned h = (unsigned)(c->p[at + 1] << 8 | c->p[at]);
    long w = t1_pc_words(at, target);
    if ((h & 0xf800u) != op)
        t1_refuse(insn, "a patch of a different instruction at offset", at);
    if (w < 0)
        return 0;
    patch_hw(c, at, (h & 0xff00u) | (unsigned)w);
    return 1;
}

int t1_patch_adr(struct code *c, int at, int target)
{
    return t1_patch_pc8(c, at, target, 0xa000u, "adr");
}

int t1_patch_ldr_lit(struct code *c, int at, int target)
{
    return t1_patch_pc8(c, at, target, 0x4800u, "ldr (literal)");
}

/* LDMIA/STMIA Rn{!}, {list} (T1): 1100 L Rn list, r0-r7, at least one.
 * There is no W bit: a store always writes the base back, and a load does
 * exactly when the base is not in the list (the loaded value wins). A
 * store of the base itself is defined only when it is the lowest register
 * in the list. 0 for a list the form cannot hold. */
int t1_ldm_stm(struct code *c, int rn, unsigned mask, int load)
{
    t1_lo(load ? "ldm" : "stm", rn);
    if (!mask || (mask & ~0xffu))
        return 0;
    if (!load && (mask & (1u << rn)) && (mask & ((1u << rn) - 1u)))
        return 0;
    hw(c, (load ? 0xc800u : 0xc000u) | ((unsigned)rn << 8) | mask);
    return 1;
}

/* PUSH {list} / POP {list} (T1): 1011 010 M list and 1011 110 P list --
 * r0-r7 plus lr (push) or pc (pop), and nothing else: r8-r11 are saved by
 * moving them into low registers first. t_push's and t_pop's 16-bit
 * forms. The offset of the instruction, for t_patch_push, or -1 for a
 * list the form cannot hold. */
int t1_push(struct code *c, unsigned mask)
{
    if (!mask || (mask & ~(0xffu | (1u << T_LR))))
        return -1;
    return t_push(c, mask);
}

int t1_pop(struct code *c, unsigned mask)
{
    if (!mask || (mask & ~(0xffu | (1u << T_PC))))
        return -1;
    return t_pop(c, mask);
}

/* UDF #imm8 (T1): 1101 1110 imm8 -- B<c> with the condition 1110, as SVC
 * (t_svc) is 1111. Permanently undefined, which is what IR_UD2 means. */
int t1_udf(struct code *c, int imm8)
{
    if (imm8 < 0 || imm8 > 255)
        return 0;
    hw(c, 0xde00u | (unsigned)imm8);
    return 1;
}
