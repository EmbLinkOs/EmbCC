/* SPARC V8 instruction encoding. See emit.h for the shape. */
#include "emit.h"

#include "../../driver/util.h"

static const char *const reg_names[32] = {
    "g0", "g1", "g2", "g3", "g4", "g5", "g6", "g7",
    "o0", "o1", "o2", "o3", "o4", "o5", "sp", "o7",
    "l0", "l1", "l2", "l3", "l4", "l5", "l6", "l7",
    "i0", "i1", "i2", "i3", "i4", "i5", "fp", "i7"
};

const char *sparc_reg_name(int r)
{
    return r >= 0 && r < 32 ? reg_names[r] : "?";
}

/* op = 10's op3 values not in emit.h's arithmetic enum, and op = 11's
 * (memory), as the SPARC V8 manual's appendix F names them. */
enum {
    O3_RDY = 0x28, O3_RDPSR = 0x29, O3_RDWIM = 0x2a, O3_RDTBR = 0x2b,
    O3_WRY = 0x30, O3_WRPSR = 0x31, O3_WRWIM = 0x32, O3_WRTBR = 0x33,
    O3_JMPL = 0x38, O3_RETT = 0x39, O3_TICC = 0x3a, O3_FLUSH = 0x3b,
    O3_SAVE = 0x3c, O3_RESTORE = 0x3d
};
enum {
    M_LD = 0x00, M_LDUB = 0x01, M_LDUH = 0x02, M_LDD = 0x03, M_ST = 0x04,
    M_STB = 0x05, M_STH = 0x06, M_STD = 0x07, M_LDSB = 0x09, M_LDSH = 0x0a,
    M_LDSTUB = 0x0d, M_SWAP = 0x0f, M_CASA = 0x3c
};
enum { OP2_UNIMP = 0, OP2_BICC = 2, OP2_SETHI = 4 };

static void need_reg(int r)
{
    if (r < 0 || r > 31)
        internal_error("sparc: register %d is not %%g0-%%i7", r);
}

static void need_field(long long v, long long lo, long long hi,
                       const char *what)
{
    if (v < lo || v > hi)
        internal_error("sparc: %s %lld is outside %lld..%lld", what, v, lo,
                       hi);
}

int sparc_simm13_ok(long long v)
{
    return v >= -4096 && v <= 4095;
}

unsigned long sparc_enc_rr(int op, int rd, int op3, int rs1, int rs2)
{
    need_reg(rd); need_reg(rs1); need_reg(rs2);
    need_field(op, 2, 3, "format-3 op");
    need_field(op3, 0, 63, "op3");
    return ((unsigned long)op << 30) | ((unsigned long)rd << 25) |
           ((unsigned long)op3 << 19) | ((unsigned long)rs1 << 14) |
           (unsigned long)rs2;
}

unsigned long sparc_enc_ri(int op, int rd, int op3, int rs1, long long simm)
{
    need_reg(rd); need_reg(rs1);
    need_field(op, 2, 3, "format-3 op");
    need_field(op3, 0, 63, "op3");
    need_field(simm, -4096, 4095, "simm13");
    return ((unsigned long)op << 30) | ((unsigned long)rd << 25) |
           ((unsigned long)op3 << 19) | ((unsigned long)rs1 << 14) |
           (1UL << 13) | ((unsigned long)simm & 0x1fffUL);
}

unsigned long sparc_enc_f2(int rd, int op2, unsigned long imm22)
{
    need_reg(rd);
    need_field(op2, 0, 7, "op2");
    need_field((long long)imm22, 0, 0x3fffff, "imm22");
    return ((unsigned long)rd << 25) | ((unsigned long)op2 << 22) | imm22;
}

void sparc_put_word(unsigned char *p, unsigned long w)
{
    for (int b = 0; b < 4; b++)
        p[3 - b] = (unsigned char)(w >> (8 * b));
}

unsigned long sparc_get_word(const unsigned char *p)
{
    unsigned long w = 0;
    for (int b = 0; b < 4; b++)
        w |= (unsigned long)p[3 - b] << (8 * b);
    return w;
}

void sparc_w(struct code *c, unsigned long w)
{
    unsigned char b[4];
    sparc_put_word(b, w & 0xffffffffUL);
    for (int k = 0; k < 4; k++)
        code_byte(c, b[k]);
}

unsigned long sparc_rdw(const struct code *c, int at)
{
    return sparc_get_word(c->p + at);
}

void sparc_wrw(struct code *c, int at, unsigned long w)
{
    sparc_put_word(c->p + at, w & 0xffffffffUL);
}

/* ---- arithmetic -------------------------------------------------------- */

static int alu_op3(int op3)
{
    return (op3 >= SP_ADD && op3 <= SP_SDIVCC && op3 != 0x09 &&
            op3 != 0x0d && op3 != 0x19 && op3 != 0x1d) ||
           op3 == SP_SLL || op3 == SP_SRL || op3 == SP_SRA;
}

void sparc_alu(struct code *c, int op3, int rd, int rs1, int rs2)
{
    if (!alu_op3(op3))
        internal_error("sparc: op3 0x%x is not an arithmetic operation", op3);
    sparc_w(c, sparc_enc_rr(2, rd, op3, rs1, rs2));
}

void sparc_alu_imm(struct code *c, int op3, int rd, int rs1, long long imm)
{
    if (!alu_op3(op3))
        internal_error("sparc: op3 0x%x is not an arithmetic operation", op3);
    if (op3 == SP_SLL || op3 == SP_SRL || op3 == SP_SRA)
        need_field(imm, 0, 31, "shift count");
    sparc_w(c, sparc_enc_ri(2, rd, op3, rs1, imm));
}

/* ---- moves and constants ---------------------------------------------- */

void sparc_nop(struct code *c) { sparc_w(c, 0x01000000UL); }

void sparc_mov(struct code *c, int rd, int rs)
{
    sparc_alu(c, SP_OR, rd, SP_G0, rs);
}

void sparc_sethi(struct code *c, int rd, unsigned long imm22)
{
    sparc_w(c, sparc_enc_f2(rd, OP2_SETHI, imm22));
}

void sparc_li(struct code *c, int rd, long long v)
{
    unsigned long u = (unsigned long)v & 0xffffffffUL;
    long long s = (long long)(int)(unsigned int)u;
    if (sparc_simm13_ok(s)) {
        sparc_alu_imm(c, SP_OR, rd, SP_G0, s);
        return;
    }
    sparc_sethi(c, rd, u >> 10);
    if (u & 0x3ff)
        sparc_alu_imm(c, SP_OR, rd, rd, (long long)(u & 0x3ff));
}

int sparc_li_len(long long v)
{
    unsigned long u = (unsigned long)v & 0xffffffffUL;
    long long s = (long long)(int)(unsigned int)u;
    if (sparc_simm13_ok(s))
        return 4;
    return (u & 0x3ff) ? 8 : 4;
}

/* ---- memory ------------------------------------------------------------- */

static int load_op3(int size, int sign)
{
    switch (size) {
    case 1: return sign ? M_LDSB : M_LDUB;
    case 2: return sign ? M_LDSH : M_LDUH;
    case 4: return M_LD;
    case 8: return M_LDD;
    default:
        internal_error("sparc: a %d-byte load", size);
        return 0;
    }
}

static int store_op3(int size)
{
    switch (size) {
    case 1: return M_STB;
    case 2: return M_STH;
    case 4: return M_ST;
    case 8: return M_STD;
    default:
        internal_error("sparc: a %d-byte store", size);
        return 0;
    }
}

static void need_even(int rd, int size)
{
    if (size == 8 && (rd & 1))
        internal_error("sparc: ldd/std with the odd register %d", rd);
}

void sparc_load(struct code *c, int rd, int base, int off, int size, int sign)
{
    int op3 = load_op3(size, sign);
    need_even(rd, size);
    sparc_w(c, sparc_enc_ri(3, rd, op3, base, off));
}

void sparc_store(struct code *c, int rd, int base, int off, int size)
{
    int op3 = store_op3(size);
    need_even(rd, size);
    sparc_w(c, sparc_enc_ri(3, rd, op3, base, off));
}

void sparc_load_rr(struct code *c, int rd, int base, int index, int size,
                   int sign)
{
    int op3 = load_op3(size, sign);
    need_even(rd, size);
    sparc_w(c, sparc_enc_rr(3, rd, op3, base, index));
}

void sparc_store_rr(struct code *c, int rd, int base, int index, int size)
{
    int op3 = store_op3(size);
    need_even(rd, size);
    sparc_w(c, sparc_enc_rr(3, rd, op3, base, index));
}

void sparc_ldstub(struct code *c, int rd, int base, int off)
{
    sparc_w(c, sparc_enc_ri(3, rd, M_LDSTUB, base, off));
}

void sparc_swap(struct code *c, int rd, int base, int off)
{
    sparc_w(c, sparc_enc_ri(3, rd, M_SWAP, base, off));
}

void sparc_casa(struct code *c, int rs1, int asi, int rs2, int rd)
{
    need_field(asi, 0, 255, "asi");
    sparc_w(c, sparc_enc_rr(3, rd, M_CASA, rs1, rs2) |
               ((unsigned long)asi << 5));
}

/* stbar is `rd %asr15, %g0` */
void sparc_stbar(struct code *c)
{
    sparc_w(c, sparc_enc_rr(2, SP_G0, O3_RDY, 15, SP_G0));
}

void sparc_flush(struct code *c, int rs1, int off)
{
    sparc_w(c, sparc_enc_ri(2, SP_G0, O3_FLUSH, rs1, off));
}

/* ---- state registers ---------------------------------------------------- */

void sparc_rdy(struct code *c, int rd)
{
    sparc_w(c, sparc_enc_rr(2, rd, O3_RDY, SP_G0, SP_G0));
}

void sparc_rdpsr(struct code *c, int rd)
{
    sparc_w(c, sparc_enc_rr(2, rd, O3_RDPSR, SP_G0, SP_G0));
}

void sparc_rdwim(struct code *c, int rd)
{
    sparc_w(c, sparc_enc_rr(2, rd, O3_RDWIM, SP_G0, SP_G0));
}

void sparc_rdtbr(struct code *c, int rd)
{
    sparc_w(c, sparc_enc_rr(2, rd, O3_RDTBR, SP_G0, SP_G0));
}

/* The writes store rs1 XOR rs2 -- `wr %g0, r, %y` writes r. */
void sparc_wry(struct code *c, int rs1, int rs2)
{
    sparc_w(c, sparc_enc_rr(2, SP_G0, O3_WRY, rs1, rs2));
}

void sparc_wrpsr(struct code *c, int rs1, int rs2)
{
    sparc_w(c, sparc_enc_rr(2, SP_G0, O3_WRPSR, rs1, rs2));
}

void sparc_wrwim(struct code *c, int rs1, int rs2)
{
    sparc_w(c, sparc_enc_rr(2, SP_G0, O3_WRWIM, rs1, rs2));
}

void sparc_wrtbr(struct code *c, int rs1, int rs2)
{
    sparc_w(c, sparc_enc_rr(2, SP_G0, O3_WRTBR, rs1, rs2));
}

/* ---- control flow ------------------------------------------------------- */

int sparc_cond_invert(int cond)
{
    need_field(cond, 0, 15, "condition");
    return cond ^ 8;           /* bn/ba, be/bne, ble/bg, ... pair at 8 apart */
}

unsigned long sparc_enc_branch(int cond, int annul, long off)
{
    need_field(cond, 0, 15, "condition");
    if (off & 3)
        internal_error("sparc: branch displacement %ld is not a multiple of 4",
                       off);
    need_field(off, -8388608L, 8388604L, "branch displacement");
    return ((unsigned long)(annul ? 1 : 0) << 29) |
           ((unsigned long)cond << 25) | ((unsigned long)OP2_BICC << 22) |
           ((unsigned long)(off / 4) & 0x3fffffUL);
}

int sparc_b_placeholder(struct code *c, int cond, int annul)
{
    int at = c->len;
    sparc_w(c, sparc_enc_branch(cond, annul, 0));
    return at;
}

int sparc_patch_b(struct code *c, int at, int target)
{
    long off = (long)target - (long)at;
    unsigned long w = sparc_rdw(c, at);
    if (off < -8388608L || off > 8388604L)
        return 0;
    w = (w & ~0x3fffffUL) | ((unsigned long)(off / 4) & 0x3fffffUL);
    sparc_wrw(c, at, w);
    return 1;
}

unsigned long sparc_enc_call(long off)
{
    if (off & 3)
        internal_error("sparc: call displacement %ld is not a multiple of 4",
                       off);
    return (1UL << 30) | ((unsigned long)(off / 4) & 0x3fffffffUL);
}

void sparc_call(struct code *c) { sparc_w(c, sparc_enc_call(0)); }

void sparc_jmpl(struct code *c, int rd, int rs1, long long off)
{
    sparc_w(c, sparc_enc_ri(2, rd, O3_JMPL, rs1, off));
}

void sparc_save(struct code *c, int rd, int rs1, long long imm)
{
    sparc_w(c, sparc_enc_ri(2, rd, O3_SAVE, rs1, imm));
}

void sparc_save_rr(struct code *c, int rd, int rs1, int rs2)
{
    sparc_w(c, sparc_enc_rr(2, rd, O3_SAVE, rs1, rs2));
}

void sparc_restore(struct code *c, int rd, int rs1, int rs2)
{
    sparc_w(c, sparc_enc_rr(2, rd, O3_RESTORE, rs1, rs2));
}

void sparc_rett(struct code *c, int rs1, long long off)
{
    sparc_w(c, sparc_enc_ri(2, SP_G0, O3_RETT, rs1, off));
}

void sparc_trap(struct code *c, int cond, int rs1, int imm7)
{
    need_field(cond, 0, 15, "condition");
    need_field(imm7, 0, 127, "trap number");
    need_reg(rs1);
    sparc_w(c, (2UL << 30) | ((unsigned long)cond << 25) |
               ((unsigned long)O3_TICC << 19) | ((unsigned long)rs1 << 14) |
               (1UL << 13) | (unsigned long)imm7);
}

void sparc_unimp(struct code *c, unsigned long imm22)
{
    sparc_w(c, sparc_enc_f2(SP_G0, OP2_UNIMP, imm22));
}
