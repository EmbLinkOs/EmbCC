/* Thumb-2 encoding for ARMv7-M. See emit.h for the model; every encoder
 * here is checked against llvm-objdump by tools/thumbcheck. */
#include "emit.h"

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

/* The 12-bit field that expands to `v`, or -1. */
static int encode_imm(unsigned long v)
{
    unsigned want = (unsigned)(v & 0xffffffffUL);
    for (unsigned e = 0; e < 4096; e++)
        if (expand_imm(e) == want)
            return (int)e;
    return -1;
}

int t_imm_ok(long imm) { return encode_imm((unsigned long)imm) >= 0; }

/* i:imm3:imm8 split into the two halfwords' fields. */
static unsigned imm_i(int e)    { return (unsigned)(e >> 11) & 1; }
static unsigned imm_hi3(int e)  { return (unsigned)(e >> 8) & 7; }
static unsigned imm_lo8(int e)  { return (unsigned)e & 0xff; }

/* ---- moves ---------------------------------------------------------- */

void t_mov_reg(struct code *c, int rd, int rm)
{
    if (rd == rm)
        return;
    hw(c, 0x4600u | (unsigned)((rd & 8) << 4) | (unsigned)(rm << 3) |
           (unsigned)(rd & 7));
}

void t_mvn_reg(struct code *c, int rd, int rm, int s)
{
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

void t_mov_imm(struct code *c, int rd, long imm, int s)
{
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
    movw(c, rd, (unsigned)(v & 0xffff), 0);
    movw(c, rd, (unsigned)(v >> 16), 1);
}

int t_mov_addr(struct code *c, int rd, unsigned long value)
{
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

int t_alu_imm(struct code *c, int op, int rd, int rn, long imm, int s)
{
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

void t_addw(struct code *c, int rd, int rn, long imm) { addsubw(c, rd, rn, imm, 0); }
void t_subw(struct code *c, int rd, int rn, long imm) { addsubw(c, rd, rn, imm, 1); }

void t_shift_imm(struct code *c, int op, int rd, int rm, int sh, int s)
{
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
    if (low(rd) && low(rn) && rd == rm) {
        hw(c, 0x4340u | (unsigned)(rn << 3) | (unsigned)rd);   /* muls */
        return;
    }
    hw2(c, 0xfb00u | (unsigned)rn, 0xf000u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_mla(struct code *c, int rd, int rn, int rm, int ra)
{
    hw2(c, 0xfb00u | (unsigned)rn,
           (unsigned)(ra << 12) | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_mls(struct code *c, int rd, int rn, int rm, int ra)
{
    hw2(c, 0xfb00u | (unsigned)rn,
           (unsigned)(ra << 12) | (unsigned)(rd << 8) | 0x10u | (unsigned)rm);
}

void t_div(struct code *c, int rd, int rn, int rm, int sign)
{
    hw2(c, (sign ? 0xfb90u : 0xfbb0u) | (unsigned)rn,
           0xf0f0u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_mull(struct code *c, int rdlo, int rdhi, int rn, int rm, int sign)
{
    hw2(c, (sign ? 0xfb80u : 0xfba0u) | (unsigned)rn,
           (unsigned)(rdlo << 12) | (unsigned)(rdhi << 8) | (unsigned)rm);
}

void t_cmp_reg(struct code *c, int rn, int rm)
{
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
    if (low(rn) && low(rm)) {
        hw(c, 0x4200u | (unsigned)(rm << 3) | (unsigned)rn);
        return;
    }
    hw2(c, 0xea10u | (unsigned)rn, 0x0f00u | (unsigned)rm);
}

void t_ext(struct code *c, int rd, int rm, int size, int sign)
{
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
    hw2(c, 0xfab0u | (unsigned)rm, 0xf080u | (unsigned)(rd << 8) | (unsigned)rm);
}

void t_rev(struct code *c, int rd, int rm)
{
    if (low(rd) && low(rm)) {
        hw(c, 0xba00u | (unsigned)(rm << 3) | (unsigned)rd);
        return;
    }
    hw2(c, 0xfa90u | (unsigned)rm, 0xf080u | (unsigned)(rd << 8) | (unsigned)rm);
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

int t_ldst_imm(struct code *c, int rt, int rn, long off, int size, int sign,
               int store)
{
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

void t_ldst_reg(struct code *c, int rt, int rn, int rm, int shift, int size,
                int sign, int store)
{
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

int t_push(struct code *c, unsigned mask)
{
    int at = c->len;
    hw2(c, 0xe92du, mask & 0x5fffu);
    return at;
}

int t_pop(struct code *c, unsigned mask)
{
    int at = c->len;
    hw2(c, 0xe8bdu, mask & 0xdfffu);
    return at;
}

void t_patch_push(struct code *c, int at, unsigned mask)
{
    patch_hw(c, at + 2, mask & 0x5fffu);
}

void t_patch_pop(struct code *c, int at, unsigned mask)
{
    patch_hw(c, at + 2, mask & 0xdfffu);
}

/* ---- control flow ---------------------------------------------------- */

int t_b(struct code *c)     { int at = c->len; hw2(c, 0xf000u, 0x9000u); return at; }
int t_bl(struct code *c)    { int at = c->len; hw2(c, 0xf000u, 0xd000u); return at; }

int t_bcond(struct code *c, int cond)
{
    int at = c->len;
    hw2(c, 0xf000u | (unsigned)(cond << 6), 0x8000u);
    return at;
}

/* The ±16MB form, shared by b.w and bl: S:I1:I2:imm10:imm11, where I1 and
 * I2 are stored as J1 = NOT(I1 XOR S) and J2 = NOT(I2 XOR S). That
 * double negation is the trap in this encoding — it exists so a short
 * forward branch has J1 = J2 = 1 and looks like the older ARM form. */
static void patch_b24(struct code *c, int at, int target, unsigned keep)
{
    long off = (long)target - (long)at - 4;
    unsigned long v = (unsigned long)off >> 1;
    unsigned s = (unsigned)((v >> 23) & 1);
    unsigned i1 = (unsigned)((v >> 22) & 1), i2 = (unsigned)((v >> 21) & 1);
    unsigned j1 = (~(i1 ^ s)) & 1, j2 = (~(i2 ^ s)) & 1;
    patch_hw(c, at, 0xf000u | (s << 10) | (unsigned)((v >> 11) & 0x3ff));
    patch_hw(c, at + 2, keep | (j1 << 13) | (j2 << 11) |
                        (unsigned)(v & 0x7ff));
}

void t_patch_b(struct code *c, int at, int target)  { patch_b24(c, at, target, 0x9000u); }
void t_patch_bl(struct code *c, int at, int target) { patch_b24(c, at, target, 0xd000u); }

/* The ±1MB conditional form: S:J2:J1:imm6:imm11, with J1 and J2 stored
 * straight rather than through the exclusive-or above. */
void t_patch_bcond(struct code *c, int at, int target)
{
    long off = (long)target - (long)at - 4;
    unsigned long v = (unsigned long)off >> 1;
    unsigned cond = (unsigned)((c->p[at + 1] << 8 | c->p[at]) >> 6) & 0xf;
    unsigned s = (unsigned)((v >> 19) & 1);
    unsigned j2 = (unsigned)((v >> 18) & 1), j1 = (unsigned)((v >> 17) & 1);
    patch_hw(c, at, 0xf000u | (s << 10) | (cond << 6) |
                    (unsigned)((v >> 11) & 0x3f));
    patch_hw(c, at + 2, 0x8000u | (j1 << 13) | (j2 << 11) |
                        (unsigned)(v & 0x7ff));
}

void t_bx(struct code *c, int rm)  { hw(c, 0x4700u | (unsigned)(rm << 3)); }
void t_blx(struct code *c, int rm) { hw(c, 0x4780u | (unsigned)(rm << 3)); }
void t_nop(struct code *c)         { hw(c, 0xbf00u); }

void t_it(struct code *c, int cond, int nthen, unsigned pattern)
{
    /* The mask is the then/else pattern followed by a 1 and then zeros;
     * for one instruction that is 0b1000. */
    unsigned mask = 0;
    for (int k = 0; k < nthen; k++) {
        unsigned bit = (pattern >> (nthen - 1 - k)) & 1;
        /* Bit 0 of the pattern means "same condition as the first". */
        mask |= (bit ^ ((unsigned)cond & 1) ^ ((unsigned)cond & 1)) << (3 - k);
    }
    mask |= 1u << (3 - nthen);
    hw(c, 0xbf00u | (unsigned)(cond << 4) | mask);
}

int t_cond_invert(int cond) { return cond ^ 1; }
