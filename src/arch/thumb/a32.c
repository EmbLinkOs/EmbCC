/* The A32 encoder: see a32.h for the model, and
 * docs/internals/arm-a32-plan.md for why it sits under the Thumb encoder's
 * interface. Every form here is checked against llvm-mc by tools/a32check
 * (tests/golden/arm-a32-encoding.sh).
 *
 * The field layouts, from the ARMv7-A/R Architecture Reference Manual's
 * A5 tables, are each written once as a packer below; every instruction
 * is one of them with its opcode bits.
 */
#include <stdio.h>
#include <stdlib.h>

#include "a32.h"
#include "emit.h"

#define AL 14u

/* emit.c and this file are linked into the encoding checkers too, which
 * carry no driver: no internal_error here. */
static void a32_fail(const char *what, long v)
{
    fprintf(stderr, "embcc: internal: arm: %s (%ld)\n", what, v);
    abort();
}

/* ---- the condition queue (t_it) ----------------------------------------- */

static unsigned char g_q[4];
static int g_qn, g_qi;

void a32_it(int cond, const char *te)
{
    int k;
    if (g_qi < g_qn)
        a32_fail("an IT block opened inside another", g_qn - g_qi);
    g_q[0] = (unsigned char)cond;
    for (k = 0; te[k] && k < 3; k++)
        g_q[k + 1] = (unsigned char)(te[k] == 't' ? cond : cond ^ 1);
    g_qn = k + 1;
    g_qi = 0;
}

int a32_it_open(void) { return g_qi < g_qn; }
void a32_it_reset(void) { g_qn = g_qi = 0; }

/* The condition the call being encoded runs under. */
static unsigned cc_take(void)
{
    unsigned cc;
    if (g_qi >= g_qn)
        return AL;
    cc = g_q[g_qi++];
    if (g_qi == g_qn)
        g_qn = g_qi = 0;
    return cc;
}

static void word(struct code *c, unsigned cc, unsigned long bits)
{
    code_u32(c, ((unsigned long)cc << 28) | (bits & 0x0fffffffUL));
}

static unsigned long rd_word(const struct code *c, int at)
{
    return (unsigned long)c->p[at] | (unsigned long)c->p[at + 1] << 8 |
           (unsigned long)c->p[at + 2] << 16 | (unsigned long)c->p[at + 3] << 24;
}

/* ---- the modified immediate --------------------------------------------
 *
 * ARMExpandImm: imm8 rotated right by twice the 4-bit rotation. Found by
 * trying each rotation, smallest first, which is the encoding LLVM picks
 * when there are several (4 is #4 with no rotation, not #1 ror 30). */
int a32_encode_imm(unsigned long v)
{
    unsigned x = (unsigned)(v & 0xffffffffUL);
    for (unsigned rot = 0; rot < 16; rot++) {
        unsigned s = 2 * rot;
        unsigned imm8 = s ? ((x << s) | (x >> (32 - s))) : x;
        if (imm8 <= 0xff)
            return (int)(rot << 8 | imm8);
    }
    return -1;
}

int a32_imm_ok(long imm) { return a32_encode_imm((unsigned long)imm) >= 0; }

/* ---- data processing ---------------------------------------------------- */

enum { DP_AND = 0, DP_EOR = 1, DP_SUB = 2, DP_RSB = 3, DP_ADD = 4, DP_ADC = 5,
       DP_SBC = 6, DP_TST = 8, DP_CMP = 10, DP_CMN = 11, DP_ORR = 12,
       DP_MOV = 13, DP_BIC = 14, DP_MVN = 15 };

/* emit.h's T_OP_* (the Thumb-2 op field) to A32's opcode; -1 for ORN,
 * which A32 does not have. */
static int dp_op(int op)
{
    switch (op) {
    case T_OP_AND: return DP_AND;
    case T_OP_BIC: return DP_BIC;
    case T_OP_ORR: return DP_ORR;
    case T_OP_EOR: return DP_EOR;
    case T_OP_ADD: return DP_ADD;
    case T_OP_ADC: return DP_ADC;
    case T_OP_SBC: return DP_SBC;
    case T_OP_SUB: return DP_SUB;
    case T_OP_RSB: return DP_RSB;
    default:       return -1;
    }
}

/* cond 000 opc S Rn Rd imm5 type 0 Rm */
static void dp_reg(struct code *c, unsigned cc, int opc, int s, int rn, int rd,
                   int imm5, int type, int rm)
{
    word(c, cc, (unsigned long)opc << 21 | (unsigned long)!!s << 20 |
                (unsigned long)rn << 16 | (unsigned long)rd << 12 |
                (unsigned long)(imm5 & 31) << 7 | (unsigned long)type << 5 |
                (unsigned long)rm);
}

/* cond 001 opc S Rn Rd rot imm8 */
static void dp_imm(struct code *c, unsigned cc, int opc, int s, int rn, int rd,
                   int e)
{
    word(c, cc, 1UL << 25 | (unsigned long)opc << 21 |
                (unsigned long)!!s << 20 | (unsigned long)rn << 16 |
                (unsigned long)rd << 12 | (unsigned long)e);
}

/* cond 000 opc S Rn Rd Rs 0 type 1 Rm: the register-shifted register. */
static void dp_rsr(struct code *c, unsigned cc, int opc, int s, int rd, int rm,
                   int type, int rs)
{
    word(c, cc, (unsigned long)opc << 21 | (unsigned long)!!s << 20 |
                (unsigned long)rd << 12 | (unsigned long)rs << 8 |
                (unsigned long)type << 5 | 1UL << 4 | (unsigned long)rm);
}

static void mov_reg_cc(struct code *c, unsigned cc, int rd, int rm)
{
    dp_reg(c, cc, DP_MOV, 0, 0, rd, 0, 0, rm);
}

void a32_mov_reg(struct code *c, int rd, int rm)
{
    unsigned cc = cc_take();
    if (rd != rm)
        mov_reg_cc(c, cc, rd, rm);
}

void a32_movs_reg(struct code *c, int rd, int rm)
{
    dp_reg(c, cc_take(), DP_MOV, 1, 0, rd, 0, 0, rm);
}

int a32_movs_imm(struct code *c, int rd, long imm)
{
    unsigned cc = cc_take();
    int e = a32_encode_imm((unsigned long)imm);
    if (e < 0)
        return -1;
    dp_imm(c, cc, DP_MOV, 1, 0, rd, e);
    return 0;
}

void a32_mvn_reg(struct code *c, int rd, int rm, int s)
{
    dp_reg(c, cc_take(), DP_MVN, s, 0, rd, 0, 0, rm);
}

void a32_movw_movt(struct code *c, int rd, unsigned v, int top)
{
    unsigned cc = cc_take();
    word(c, cc, (top ? 0x03400000UL : 0x03000000UL) |
                (unsigned long)((v >> 12) & 0xf) << 16 |
                (unsigned long)rd << 12 | (v & 0xfff));
}

static void movw_cc(struct code *c, unsigned cc, int rd, unsigned v, int top)
{
    word(c, cc, (top ? 0x03400000UL : 0x03000000UL) |
                (unsigned long)((v >> 12) & 0xf) << 16 |
                (unsigned long)rd << 12 | (v & 0xfff));
}

/* rd = v, flags untouched (t_mov_imm's `s` is only ever a preference):
 * mov or mvn of a modified immediate, movw, or movw and movt. */
static void mov_imm_cc(struct code *c, unsigned cc, int rd, unsigned long v)
{
    int e;
    v &= 0xffffffffUL;
    if ((e = a32_encode_imm(v)) >= 0) {
        dp_imm(c, cc, DP_MOV, 0, 0, rd, e);
        return;
    }
    if ((e = a32_encode_imm(~v & 0xffffffffUL)) >= 0) {
        dp_imm(c, cc, DP_MVN, 0, 0, rd, e);
        return;
    }
    movw_cc(c, cc, rd, (unsigned)(v & 0xffff), 0);
    if (v > 0xffff)
        movw_cc(c, cc, rd, (unsigned)(v >> 16), 1);
}

void a32_mov_imm(struct code *c, int rd, long imm, int s)
{
    (void)s;
    mov_imm_cc(c, cc_take(), rd, (unsigned long)imm);
}

int a32_mov_addr(struct code *c, int rd, unsigned long value)
{
    unsigned cc = cc_take();
    int at = c->len;
    movw_cc(c, cc, rd, (unsigned)(value & 0xffff), 0);
    movw_cc(c, cc, rd, (unsigned)((value >> 16) & 0xffff), 1);
    return at;
}

int a32_ldr_const(struct code *c, int rd, unsigned long v)
{
    int e;
    v &= 0xffffffffUL;
    if (rd == 13 || rd == 15)
        return 0;
    if ((e = a32_encode_imm(v)) >= 0) {
        dp_imm(c, cc_take(), DP_MOV, 0, 0, rd, e);
        return 1;
    }
    if ((e = a32_encode_imm(~v & 0xffffffffUL)) >= 0) {
        dp_imm(c, cc_take(), DP_MVN, 0, 0, rd, e);
        return 1;
    }
    if (v <= 0xffff) {
        movw_cc(c, cc_take(), rd, (unsigned)v, 0);
        return 1;
    }
    return 0;
}

void a32_alu_reg(struct code *c, int op, int rd, int rn, int rm, int s)
{
    int opc = dp_op(op);
    unsigned cc = cc_take();
    if (opc < 0)
        a32_fail("an ORN, which A32 does not have", op);
    dp_reg(c, cc, opc, s, rn, rd, 0, 0, rm);
}

void a32_alu_reg_shift(struct code *c, int op, int rd, int rn, int rm,
                       int type, int amount, int s)
{
    int opc = dp_op(op);
    unsigned cc = cc_take();
    if (opc < 0)
        a32_fail("an ORN, which A32 does not have", op);
    if (amount < 1 || amount > 32 || (amount == 32 && type != T_SH_LSR &&
                                      type != T_SH_ASR))
        a32_fail("a shifted operand's amount", amount);
    dp_reg(c, cc, opc, s, rn, rd, amount & 31, type, rm);
}

/* rd = rn <op> imm. Beyond the immediate itself, the equivalent forms
 * where the flags are not asked for: ADD of a negative as SUB, AND of a
 * mask whose complement rotates as BIC, and ORN -- which A32 lacks -- as
 * ORR of the complement. 0, writing nothing, when none rotates. */
static int alu_imm_cc(struct code *c, unsigned cc, int op, int rd, int rn,
                      long imm, int s)
{
    unsigned long v = (unsigned long)imm & 0xffffffffUL;
    unsigned long nv = ~v & 0xffffffffUL, neg = (0UL - v) & 0xffffffffUL;
    int opc = dp_op(op), e;
    if (op == T_OP_ORN) {
        if ((e = a32_encode_imm(nv)) < 0)
            return 0;
        dp_imm(c, cc, DP_ORR, s, rn, rd, e);
        return 1;
    }
    if (opc < 0)
        return 0;
    if ((e = a32_encode_imm(v)) >= 0) {
        dp_imm(c, cc, opc, s, rn, rd, e);
        return 1;
    }
    if (s)
        return 0;
    if ((op == T_OP_ADD || op == T_OP_SUB) &&
        (e = a32_encode_imm(neg)) >= 0) {
        dp_imm(c, cc, op == T_OP_ADD ? DP_SUB : DP_ADD, 0, rn, rd, e);
        return 1;
    }
    if ((op == T_OP_AND || op == T_OP_BIC) &&
        (e = a32_encode_imm(nv)) >= 0) {
        dp_imm(c, cc, op == T_OP_AND ? DP_BIC : DP_AND, 0, rn, rd, e);
        return 1;
    }
    return 0;
}

int a32_alu_imm(struct code *c, int op, int rd, int rn, long imm, int s)
{
    return alu_imm_cc(c, cc_take(), op, rd, rn, imm, s);
}

/* rd = rn + imm (or - imm), imm 0..4095, flags untouched: what Thumb's
 * addw/subw say in one instruction. One rotated immediate when it is one,
 * else bits 11..4 and bits 3..0, each of which rotates. */
static void addsubw_cc(struct code *c, unsigned cc, int rd, int rn, long imm,
                       int sub)
{
    unsigned v = (unsigned)imm & 0xfffu;
    int opc = sub ? DP_SUB : DP_ADD, e;
    if ((e = a32_encode_imm(v)) >= 0) {
        dp_imm(c, cc, opc, 0, rn, rd, e);
        return;
    }
    dp_imm(c, cc, opc, 0, rn, rd, a32_encode_imm(v & 0xff0u));
    dp_imm(c, cc, opc, 0, rd, rd, a32_encode_imm(v & 0xfu));
}

void a32_addsubw(struct code *c, int rd, int rn, long imm, int sub)
{
    addsubw_cc(c, cc_take(), rd, rn, imm, sub);
}

void a32_shift_imm(struct code *c, int op, int rd, int rm, int sh, int s)
{
    unsigned cc = cc_take();
    if (op == T_SH_LSL && sh == 0) {
        if (s)
            dp_reg(c, cc, DP_MOV, 1, 0, rd, 0, 0, rm);
        else if (rd != rm)
            mov_reg_cc(c, cc, rd, rm);
        return;
    }
    /* LSR and ASR by 32 are the field's 0; ROR by 0 would be RRX */
    if (sh < 1 || sh > 32 || (sh == 32 && op != T_SH_LSR && op != T_SH_ASR))
        a32_fail("a shift amount", sh);
    dp_reg(c, cc, DP_MOV, s, 0, rd, sh & 31, op, rm);
}

void a32_shift_reg(struct code *c, int op, int rd, int rn, int rm, int s)
{
    dp_rsr(c, cc_take(), DP_MOV, s, rd, rn, op, rm);
}

void a32_cmp_reg(struct code *c, int rn, int rm)
{
    dp_reg(c, cc_take(), DP_CMP, 1, rn, 0, 0, 0, rm);
}

/* cmp rn, #imm; or cmn rn, #-imm, which sets the same flags whenever imm
 * itself does not rotate (INT_MIN, the one value where they differ,
 * does). */
void a32_cmp_imm(struct code *c, int rn, long imm)
{
    unsigned cc = cc_take();
    unsigned long v = (unsigned long)imm & 0xffffffffUL;
    int e;
    if ((e = a32_encode_imm(v)) >= 0) {
        dp_imm(c, cc, DP_CMP, 1, rn, 0, e);
        return;
    }
    if ((e = a32_encode_imm((0UL - v) & 0xffffffffUL)) >= 0) {
        dp_imm(c, cc, DP_CMN, 1, rn, 0, e);
        return;
    }
    a32_fail("a compare with an immediate that does not rotate", imm);
}

void a32_tst_reg(struct code *c, int rn, int rm)
{
    dp_reg(c, cc_take(), DP_TST, 1, rn, 0, 0, 0, rm);
}

int a32_tst_imm(struct code *c, int rn, long imm)
{
    unsigned cc = cc_take();
    int e = a32_encode_imm((unsigned long)imm);
    if (e < 0)
        return 0;
    dp_imm(c, cc, DP_TST, 1, rn, 0, e);
    return 1;
}

/* ---- multiply and divide ------------------------------------------------ */

/* MUL: cond 0000 000S Rd 0000 Rm 1001 Rn */
void a32_mul(struct code *c, int rd, int rn, int rm)
{
    word(c, cc_take(), (unsigned long)rd << 16 | (unsigned long)rm << 8 |
                       0x90UL | (unsigned long)rn);
}

/* MLA: cond 0000 0010 Rd Ra Rm 1001 Rn; MLS: cond 0000 0110 ... */
void a32_mla(struct code *c, int rd, int rn, int rm, int ra, int sub)
{
    word(c, cc_take(), (sub ? 0x00600000UL : 0x00200000UL) |
                       (unsigned long)rd << 16 | (unsigned long)ra << 12 |
                       (unsigned long)rm << 8 | 0x90UL | (unsigned long)rn);
}

/* UMULL/SMULL: cond 0000 1U00 RdHi RdLo Rm 1001 Rn */
void a32_mull(struct code *c, int rdlo, int rdhi, int rn, int rm, int sign)
{
    word(c, cc_take(), (sign ? 0x00C00000UL : 0x00800000UL) |
                       (unsigned long)rdhi << 16 | (unsigned long)rdlo << 12 |
                       (unsigned long)rm << 8 | 0x90UL | (unsigned long)rn);
}

/* SDIV/UDIV: cond 0111 0U01 Rd 1111 Rm 0001 Rn -- the ARMv7VE (Cortex-A7,
 * A15) instructions. Base ARMv7-A has neither, and the code generator
 * calls __aeabi_idiv instead; this exists for the encoder's referee and
 * for inline asm on a part that has them. */
void a32_div(struct code *c, int rd, int rn, int rm, int sign)
{
    word(c, cc_take(), (sign ? 0x0710F010UL : 0x0730F010UL) |
                       (unsigned long)rd << 16 | (unsigned long)rm << 8 |
                       (unsigned long)rn);
}

/* ---- extends and bit operations ------------------------------------------ */

void a32_ext(struct code *c, int rd, int rm, int size, int sign)
{
    unsigned long base = size == 1 ? (sign ? 0x06AF0070UL : 0x06EF0070UL)
                                   : (sign ? 0x06BF0070UL : 0x06FF0070UL);
    word(c, cc_take(), base | (unsigned long)rd << 12 | (unsigned long)rm);
}

void a32_bitop(struct code *c, int which, int rd, int rm)
{
    static const unsigned long op[4] = {
        0x016F0F10UL,           /* clz   */
        0x06BF0F30UL,           /* rev   */
        0x06BF0FB0UL,           /* rev16 */
        0x06FF0F30UL            /* rbit  */
    };
    word(c, cc_take(), op[which] | (unsigned long)rd << 12 | (unsigned long)rm);
}

/* UBFX/SBFX: cond 0111 1U1 widthm1 Rd lsb 101 Rn */
void a32_bfx(struct code *c, int rd, int rn, int lsb, int width, int sign)
{
    if (lsb < 0 || width < 1 || lsb + width > 32)
        a32_fail("a bit-field outside the word", lsb * 100 + width);
    word(c, cc_take(), (sign ? 0x07A00050UL : 0x07E00050UL) |
                       (unsigned long)(width - 1) << 16 |
                       (unsigned long)rd << 12 | (unsigned long)lsb << 7 |
                       (unsigned long)rn);
}

/* ---- loads and stores ----------------------------------------------------
 *
 * Two families. A word or an unsigned byte: cond 01 I P U B W L Rn Rt and a
 * 12-bit offset (I = 0) or a shifted register (I = 1). A halfword, a signed
 * byte or halfword, or a doubleword -- the "extra" loads and stores: cond
 * 000 P U I W L Rn Rt imm4H 1 op2 1 imm4L, an 8-bit offset (I = 1) or an
 * unshifted register (I = 0). op2 01 is H, 10 SB (or LDRD with L = 0),
 * 11 SH (or STRD with L = 0). */
static int wide_form(int size, int sign) { return size == 4 || (size == 1 && !sign); }

static unsigned long extra_op2(int size, int sign)
{
    if (size == 2 && !sign) return 1;
    if (size == 1) return 2;
    return 3;                                     /* signed halfword */
}

static void ldst_word(struct code *c, unsigned cc, int rt, int rn, long off,
                      int size, int store, int p, int w)
{
    unsigned long u = off >= 0, mag = (unsigned long)(off >= 0 ? off : -off);
    word(c, cc, 0x04000000UL | (unsigned long)p << 24 | u << 23 |
                (unsigned long)(size == 1) << 22 | (unsigned long)w << 21 |
                (unsigned long)!store << 20 | (unsigned long)rn << 16 |
                (unsigned long)rt << 12 | mag);
}

static void ldst_extra(struct code *c, unsigned cc, int rt, int rn, long off,
                       unsigned long op2, int load, int p, int w)
{
    unsigned long u = off >= 0, mag = (unsigned long)(off >= 0 ? off : -off);
    word(c, cc, 0x00400090UL | (unsigned long)p << 24 | u << 23 |
                (unsigned long)w << 21 | (unsigned long)load << 20 |
                (unsigned long)rn << 16 | (unsigned long)rt << 12 |
                (mag >> 4) << 8 | op2 << 5 | (mag & 15));
}

int a32_ldst_imm(struct code *c, int rt, int rn, long off, int size, int sign,
                 int store)
{
    unsigned cc = cc_take();
    if (store || size >= 4)
        sign = 0;
    if (wide_form(size, sign)) {
        if (off < -4095 || off > 4095)
            return 0;
        ldst_word(c, cc, rt, rn, off, size, store, 1, 0);
        return 1;
    }
    if (off < -255 || off > 255)
        return 0;
    ldst_extra(c, cc, rt, rn, off, extra_op2(size, sign), !store, 1, 0);
    return 1;
}

/* LDRD/STRD: an EVEN register and the next one, never lr:pc. */
int a32_ldst_pair(struct code *c, int rt, int rt2, int rn, long off, int store)
{
    unsigned cc;
    if ((rt & 1) || rt2 != rt + 1 || rt >= 14 || rn == 15 ||
        off < -255 || off > 255)
        return 0;
    cc = cc_take();
    ldst_extra(c, cc, rt, rn, off, store ? 3 : 2, 0, 1, 0);
    return 1;
}

int a32_ldst_wb(struct code *c, int rt, int rn, long off, int size, int sign,
                int store, int pre)
{
    unsigned cc;
    if (store || size >= 4)
        sign = 0;
    if (rn == 15 || rn == rt)
        return 0;
    if (wide_form(size, sign) ? (off < -4095 || off > 4095)
                              : (off < -255 || off > 255))
        return 0;
    cc = cc_take();
    /* pre-indexed: P = 1, W = 1; post-indexed: P = 0, W = 0 (W = 1 there
     * would be the unprivileged LDRT/STRT) */
    if (wide_form(size, sign))
        ldst_word(c, cc, rt, rn, off, size, store, pre, pre);
    else
        ldst_extra(c, cc, rt, rn, off, extra_op2(size, sign), !store, pre, pre);
    return 1;
}

int a32_ldst_reg_ok(int shift, int size, int sign, int store)
{
    if (store || size >= 4)
        sign = 0;
    return shift == 0 || wide_form(size, sign);
}

void a32_ldst_reg(struct code *c, int rt, int rn, int rm, int shift, int size,
                  int sign, int store)
{
    unsigned cc = cc_take();
    if (store || size >= 4)
        sign = 0;
    if (wide_form(size, sign)) {
        /* cond 011 P U B W L Rn Rt imm5 type 0 Rm, P = U = 1 */
        word(c, cc, 0x07800000UL | (unsigned long)(size == 1) << 22 |
                    (unsigned long)!store << 20 | (unsigned long)rn << 16 |
                    (unsigned long)rt << 12 | (unsigned long)shift << 7 |
                    (unsigned long)rm);
        return;
    }
    if (shift)
        a32_fail("a halfword or signed access through a shifted register",
                 shift);
    /* cond 000 1 1 0 0 L Rn Rt 0000 1 op2 1 Rm */
    word(c, cc, 0x01800090UL | (unsigned long)!store << 20 |
                (unsigned long)rn << 16 | (unsigned long)rt << 12 |
                extra_op2(size, sign) << 5 | (unsigned long)rm);
}

void a32_add_sp(struct code *c, int rd, long off)
{
    unsigned cc = cc_take();
    int e;
    if (off >= 0 && off <= 4095) {
        addsubw_cc(c, cc, rd, 13, off, 0);
        return;
    }
    if (off < 0 && (e = a32_encode_imm((unsigned long)-off)) >= 0) {
        dp_imm(c, cc, DP_SUB, 0, 13, rd, e);
        return;
    }
    mov_imm_cc(c, cc, rd, (unsigned long)off);
    dp_reg(c, cc, DP_ADD, 0, 13, rd, 0, 0, rd);
}

void a32_sp_adjust(struct code *c, long imm, int sub)
{
    unsigned cc = cc_take();
    if (imm >= 0 && imm <= 4095) {
        addsubw_cc(c, cc, 13, 13, imm, sub);
        return;
    }
    mov_imm_cc(c, cc, T_ACC, (unsigned long)imm);
    dp_reg(c, cc, sub ? DP_SUB : DP_ADD, 0, 13, 13, 0, 0, T_ACC);
}

/* LDM/STM: cond 100 P U 0 W L Rn list. push is STMDB sp!, pop LDMIA sp!. */
static void ldm_stm(struct code *c, unsigned cc, int rn, unsigned mask,
                    int wback, int before, int load)
{
    word(c, cc, 0x08000000UL | (unsigned long)!!before << 24 |
                (unsigned long)!before << 23 | (unsigned long)!!wback << 21 |
                (unsigned long)!!load << 20 | (unsigned long)rn << 16 |
                (mask & 0xffffUL));
}

int a32_push(struct code *c, unsigned mask)
{
    int at = c->len;
    ldm_stm(c, cc_take(), 13, mask & 0x5fffu, 1, 1, 0);
    return at;
}

int a32_pop(struct code *c, unsigned mask)
{
    int at = c->len;
    ldm_stm(c, cc_take(), 13, mask & 0xdfffu, 1, 0, 1);
    return at;
}

void a32_patch_mask(struct code *c, int at, unsigned mask)
{
    unsigned long w = rd_word(c, at);
    if ((w & 0x0e000000UL) != 0x08000000UL || !mask)
        a32_fail("patching a register list that is not an LDM/STM", at);
    code_patch32(c, at, (w & 0xffff0000UL) | (mask & 0xffffu));
}

int a32_ldm_stm(struct code *c, int rn, unsigned mask, int wback, int before,
                int load)
{
    if (!mask || rn < 0 || rn > 14 || (mask & ~0xffffu) ||
        (mask & (1u << 13)) || (!load && (mask & (1u << 15))) ||
        (wback && (mask & (1u << rn))))
        return 0;
    ldm_stm(c, cc_take(), rn, mask, wback, before, load);
    return 1;
}

/* ---- control flow -------------------------------------------------------- */

/* b / bl: cond 101 L imm24, the offset in words from the instruction + 8.
 * `cond` < 0 takes the IT queue's (or AL). */
int a32_b(struct code *c, int cond, int link)
{
    int at = c->len;
    unsigned cc = cond < 0 ? cc_take() : (unsigned)cond;
    word(c, cc, link ? 0x0B000000UL : 0x0A000000UL);
    return at;
}

int a32_patch_b(struct code *c, int at, int target)
{
    long off = (long)target - (long)at - 8;
    unsigned long w = rd_word(c, at);
    if ((off & 3) || off < -33554432L || off > 33554428L)
        return 0;
    code_patch32(c, at, (w & 0xff000000UL) |
                        (((unsigned long)off >> 2) & 0xffffffUL));
    return 1;
}

/* BX / BLX (register): cond 0001 0010 1111 1111 1111 00L1 Rm */
void a32_bx(struct code *c, int rm, int link)
{
    word(c, cc_take(), (link ? 0x012FFF30UL : 0x012FFF10UL) |
                       (unsigned long)rm);
}

void a32_hint(struct code *c, int op)
{
    word(c, cc_take(), 0x0320F000UL | (unsigned long)(op & 0xff));
}

void a32_nop(struct code *c) { a32_hint(c, T_HINT_NOP); }

/* ADR: ADD/SUB rd, pc, #imm, the pc reading as the instruction + 8. */
int a32_adr(struct code *c, int rd, long imm)
{
    unsigned long m = (unsigned long)(imm < 0 ? -imm : imm);
    int e = a32_encode_imm(m);
    if (e < 0)
        return 0;
    dp_imm(c, cc_take(), imm < 0 ? DP_SUB : DP_ADD, 0, 15, rd, e);
    return 1;
}

int a32_patch_adr(struct code *c, int at, int rd, long imm)
{
    unsigned long m = (unsigned long)(imm < 0 ? -imm : imm);
    unsigned long w = rd_word(c, at);
    int e = a32_encode_imm(m);
    if (e < 0)
        return 0;
    code_patch32(c, at, (w & 0xf0000000UL) | 1UL << 25 |
                        (unsigned long)(imm < 0 ? DP_SUB : DP_ADD) << 21 |
                        15UL << 16 | (unsigned long)rd << 12 |
                        (unsigned long)e);
    return 1;
}

/* mov<!c> rd, #0; mov<c> rd, #1 -- after a compare, rd = cond ? 1 : 0. */
void a32_setcc(struct code *c, int cond, int rd)
{
    dp_imm(c, (unsigned)cond ^ 1u, DP_MOV, 0, 0, rd, 0);
    dp_imm(c, (unsigned)cond, DP_MOV, 0, 0, rd, 1);
}

/* ---- system --------------------------------------------------------------- */

/* CPS: 1111 0001 0000 imod 0 0 0000 000 A I F 0 00000, imod 11 to disable
 * (cpsid), 10 to enable. Unconditional. */
void a32_cps(struct code *c, int disable, int mask_i, int mask_f)
{
    (void)cc_take();
    code_u32(c, 0xF1000000UL | (disable ? 3UL : 2UL) << 18 |
                (mask_i ? 1UL << 7 : 0) | (mask_f ? 1UL << 6 : 0));
}

/* DSB/DMB/ISB SY: 1111 0101 0111 1111 1111 0000 op 1111. Unconditional. */
void a32_barrier(struct code *c, int op)
{
    (void)cc_take();
    code_u32(c, 0xF57FF00FUL | (unsigned long)op << 4);
}

void a32_clrex(struct code *c)
{
    (void)cc_take();
    code_u32(c, 0xF57FF01FUL);
}

/* BKPT #imm16: cond 0001 0010 imm12 0111 imm4, the condition AL only. */
void a32_bkpt(struct code *c, int imm)
{
    (void)cc_take();
    code_u32(c, 0xE1200070UL | ((unsigned long)imm >> 4 & 0xfffUL) << 8 |
                ((unsigned long)imm & 15));
}

void a32_svc(struct code *c, long imm)
{
    word(c, cc_take(), 0x0F000000UL | ((unsigned long)imm & 0xffffffUL));
}

/* UDF #imm16: 1110 0111 1111 imm12 1111 imm4 */
void a32_udf(struct code *c, int imm)
{
    (void)cc_take();
    code_u32(c, 0xE7F000F0UL | ((unsigned long)imm >> 4 & 0xfffUL) << 8 |
                ((unsigned long)imm & 15));
}

void a32_mrs_cpsr(struct code *c, int rd)
{
    word(c, cc_take(), 0x010F0000UL | (unsigned long)rd << 12);
}

void a32_msr_cpsr(struct code *c, int fields, int rn)
{
    word(c, cc_take(), 0x0120F000UL | (unsigned long)(fields & 15) << 16 |
                       (unsigned long)rn);
}

/* MRC/MCR: cond 1110 opc1 L CRn Rt coproc opc2 1 CRm */
void a32_mrc_mcr(struct code *c, int load, int cp, int opc1, int rt, int crn,
                 int crm, int opc2)
{
    word(c, cc_take(), 0x0E000010UL | (unsigned long)(opc1 & 7) << 21 |
                       (unsigned long)!!load << 20 |
                       (unsigned long)(crn & 15) << 16 |
                       (unsigned long)rt << 12 | (unsigned long)(cp & 15) << 8 |
                       (unsigned long)(opc2 & 7) << 5 |
                       (unsigned long)(crm & 15));
}

/* LDR rt, [pc, #off]: `off` from the instruction + 8, as A32 reads pc. */
int a32_ldr_lit(struct code *c, int rt, long off)
{
    unsigned cc;
    if (off < -4095 || off > 4095)
        return 0;
    cc = cc_take();
    ldst_word(c, cc, rt, 15, off, 4, 0, 1, 0);
    return 1;
}

/* LDREX{B,H,D}: cond 0001 1 op 1 Rn Rt 1111 1001 1111; STREX{B,H,D}:
 * cond 0001 1 op 0 Rn Rd 1111 1001 Rt, op 00 word, 01 doubleword, 10 byte,
 * 11 halfword. */
static unsigned long excl_op(int size)
{
    return size == 4 ? 0 : size == 8 ? 1 : size == 1 ? 2 : 3;
}

void a32_ldrex(struct code *c, int rt, int rn, int size)
{
    if (size == 8 && ((rt & 1) || rt >= 14))
        a32_fail("ldrexd of a pair that is not an even register and the next",
                 rt);
    word(c, cc_take(), 0x01900F9FUL | excl_op(size) << 21 |
                       (unsigned long)rn << 16 | (unsigned long)rt << 12);
}

void a32_strex(struct code *c, int rd, int rt, int rn, int size)
{
    if (size == 8 && ((rt & 1) || rt >= 14))
        a32_fail("strexd of a pair that is not an even register and the next",
                 rt);
    word(c, cc_take(), 0x01800F90UL | excl_op(size) << 21 |
                       (unsigned long)rn << 16 | (unsigned long)rd << 12 |
                       (unsigned long)rt);
}

void a32_vfp_word(struct code *c, unsigned h1, unsigned h2)
{
    if ((h1 >> 12) != 0xEu)
        a32_fail("a Thumb coprocessor halfword without the 1110 prefix",
                 (long)h1);
    word(c, cc_take(), (unsigned long)(h1 & 0x0fffu) << 16 | (h2 & 0xffffu));
}
