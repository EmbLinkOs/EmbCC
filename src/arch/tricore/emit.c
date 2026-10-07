/* TriCore 1.6 instruction encoding. See emit.h for the shape. */
#include "emit.h"

#include "../../driver/util.h"

static const char *const dnames[16] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15"
};
static const char *const anames[16] = {
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "a8", "a9", "a10", "a11", "a12", "a13", "a14", "a15"
};
static const char *const enames[8] = {
    "e0", "e2", "e4", "e6", "e8", "e10", "e12", "e14"
};

const char *tc_reg_name(int file, int r)
{
    if (r < 0 || r > 15)
        return "?";
    if (file == 'a')
        return anames[r];
    if (file == 'e')
        return r & 1 ? "?" : enames[r / 2];
    return dnames[r];
}

/* The primary opcodes (op1) and the secondary ones under them, named as
 * the TriCore 1.6 architecture manual's instruction set volume names the
 * formats: OPC_<format>_<group>. */
enum {
    OPC_RR_ADDRESS = 0x01,        /* MOV.A, MOV.D, MOV.AA, ADD.A, ADDSC.A */
    OPC_RR_ACCUMULATOR = 0x0b,    /* ADD, SUB, the compares, MIN, MAX, MOV */
    OPC_RR_LOGICAL_SHIFT = 0x0f,  /* AND, OR, XOR, ..., SH, SHA, CLZ */
    OPC_RLC_ADDI = 0x1b, OPC_RLC_ADDIH = 0x9b, OPC_RLC_ADDIH_A = 0x11,
    OPC_RLC_MOV = 0x3b, OPC_RLC_MOV_U = 0xbb, OPC_RLC_MOVH = 0x7b,
    OPC_RLC_MOVH_A = 0x91, OPC_RLC_MFCR = 0x4d, OPC_RLC_MTCR = 0xcd,
    OPC_RRR2_MADD = 0x03,
    OPC_RRRR_EXTRACT = 0x17,      /* DEXTR with a register position */
    OPC_RRR_COND_SELECT = 0x2b,   /* SEL, SELN */
    OPC_RR_IDIRECT = 0x2d,        /* JI, JLI, CALLI */
    OPC_RRPW_EXTRACT = 0x37,      /* EXTR, EXTR.U, INSERT */
    OPC_RR_DIVIDE = 0x4b,         /* DIV, DIV.U */
    OPC_RC_MUL = 0x53,
    OPC_RR2_MUL = 0x73,
    OPC_RRPW_DEXTR = 0x77,
    OPC_RC_ACCUMULATOR = 0x8b,
    OPC_RC_LOGICAL_SHIFT = 0x8f,
    OPC_RC_SERVICE = 0xad,        /* SYSCALL */
    OPC_RCPW_INSERT = 0xb7,
    OPC_BO_STCTX = 0x49,          /* SWAP.W, CMPSWAP.W (short offset) */
    OPC_SYS = 0x0d,
    /* the long-offset (BOL) loads and stores */
    OPC_BOL_LD_W = 0x19, OPC_BOL_LD_A = 0x99, OPC_BOL_LD_B = 0x79,
    OPC_BOL_LD_BU = 0x39, OPC_BOL_LD_H = 0xc9, OPC_BOL_LD_HU = 0xb9,
    OPC_BOL_ST_W = 0x59, OPC_BOL_ST_A = 0xb5, OPC_BOL_ST_B = 0xe9,
    OPC_BOL_ST_H = 0xf9, OPC_BOL_LEA = 0xd9,
    /* the branches: op2 (bit 31) picks the second of each pair */
    OPC_BRR_JEQ = 0x5f, OPC_BRR_JLT = 0x3f, OPC_BRR_JGE = 0x7f,
    OPC_BRR_JEQ_A = 0x7d, OPC_BRR_JZ_A = 0xbd,
    OPC_BRC_JEQ = 0xdf, OPC_BRC_JLT = 0xbf, OPC_BRC_JGE = 0xff,
    OPC_B_J = 0x1d, OPC_B_CALL = 0x6d, OPC_B_JL = 0x5d
};
enum {                                  /* RR, under OPC_RR_ADDRESS */
    RR_MOV_AA = 0x00, RR_ADD_A = 0x01, RR_SUB_A = 0x02, RR_MOV_D = 0x4c,
    RR_ADDSC_A = 0x60, RR_MOV_A = 0x63
};
enum { RR_MOV = 0x1f };                 /* under OPC_RR_ACCUMULATOR */
enum { RR_CLZ = 0x1b };                 /* under OPC_RR_LOGICAL_SHIFT */
enum { RR_DIV = 0x20, RR_DIV_U = 0x21 };
enum { RR2_MUL32 = 0x0a, RR2_MUL64 = 0x6a, RR2_MUL64_U = 0x68 };
enum { RRR2_MADD32 = 0x0a };
enum { RRR_SEL = 0x04, RRR_SELN = 0x05 };
enum { RRPW_INSERT = 0x00, RRPW_EXTR = 0x02, RRPW_EXTR_U = 0x03 };
enum { RRRR_DEXTR = 0x04 };
enum { RR_JI = 0x03, RR_JLI = 0x02, RR_CALLI = 0x00 };
enum {                                  /* SYS */
    SYS_NOP = 0x00, SYS_DEBUG = 0x04, SYS_RET = 0x06, SYS_RFE = 0x07,
    SYS_SVLCX = 0x08, SYS_RSLCX = 0x09, SYS_ENABLE = 0x0c,
    SYS_DISABLE = 0x0d, SYS_DSYNC = 0x12, SYS_ISYNC = 0x13
};
enum { RC_SYSCALL = 0x04 };
enum { BO_SWAP_W = 0x20, BO_CMPSWAP_W = 0x23 };

static void need_reg(int r)
{
    if (r < 0 || r > 15)
        internal_error("tricore: register %d is not 0-15", r);
}

static void need_field(long long v, long long lo, long long hi,
                       const char *what)
{
    if (v < lo || v > hi)
        internal_error("tricore: %s %lld is outside %lld..%lld", what, v,
                       lo, hi);
}

static void need_even(int e)
{
    need_reg(e);
    if (e & 1)
        internal_error("tricore: e%d is not a register pair (odd)", e);
}

int tc_fits(long long v, int bits, int sign)
{
    long long half = 1LL << (bits - 1);
    return sign ? v >= -half && v < half : v >= 0 && v < 2 * half;
}

/* A value as the `bits`-wide field its instruction extends back to it. */
static unsigned field(long long v, int bits, int sign, const char *what)
{
    if (!tc_fits(v, bits, sign))
        internal_error("tricore: %s: %lld does not fit a %s %d-bit field",
                       what, v, sign ? "sign-extended" : "zero-extended",
                       bits);
    return (unsigned)(v & ((1LL << bits) - 1));
}

/* ---- the formats ------------------------------------------------------ */

static unsigned long op_d(int op1, int d)
{
    need_field(op1, 0, 255, "op1");
    need_reg(d);
    return (unsigned long)op1 | ((unsigned long)d << 28);
}

unsigned long tc_enc_rr(int op1, int op2, int d, int s1, int s2, int n)
{
    need_reg(s1); need_reg(s2);
    need_field(op2, 0, 255, "RR op2");
    need_field(n, 0, 3, "RR n");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)s2 << 12) |
           ((unsigned long)n << 16) | ((unsigned long)op2 << 20);
}

unsigned long tc_enc_rr2(int op1, int op2, int d, int s1, int s2)
{
    need_reg(s1); need_reg(s2);
    need_field(op2, 0, 4095, "RR2 op2");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)s2 << 12) |
           ((unsigned long)op2 << 16);
}

unsigned long tc_enc_rc(int op1, int op2, int d, int s1, unsigned c9)
{
    need_reg(s1);
    need_field(op2, 0, 127, "RC op2");
    need_field(c9, 0, 511, "RC const9");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)c9 << 12) |
           ((unsigned long)op2 << 21);
}

unsigned long tc_enc_rlc(int op1, int d, int s1, unsigned c16)
{
    need_reg(s1);
    need_field(c16, 0, 0xffff, "RLC const16");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)c16 << 12);
}

unsigned long tc_enc_rrr(int op1, int op2, int d, int s1, int s2, int s3,
                         int n)
{
    need_reg(s1); need_reg(s2); need_reg(s3);
    need_field(op2, 0, 15, "RRR op2");
    need_field(n, 0, 3, "RRR n");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)s2 << 12) |
           ((unsigned long)n << 16) | ((unsigned long)op2 << 20) |
           ((unsigned long)s3 << 24);
}

unsigned long tc_enc_rrr2(int op1, int op2, int d, int s1, int s2, int s3)
{
    need_reg(s1); need_reg(s2); need_reg(s3);
    need_field(op2, 0, 255, "RRR2 op2");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)s2 << 12) |
           ((unsigned long)op2 << 16) | ((unsigned long)s3 << 24);
}

unsigned long tc_enc_rrrr(int op1, int op2, int d, int s1, int s2, int s3)
{
    need_reg(s1); need_reg(s2); need_reg(s3);
    need_field(op2, 0, 7, "RRRR op2");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)s2 << 12) |
           ((unsigned long)op2 << 21) | ((unsigned long)s3 << 24);
}

unsigned long tc_enc_rrpw(int op1, int op2, int d, int s1, int s2, int pos,
                          int width)
{
    need_reg(s1); need_reg(s2);
    need_field(op2, 0, 3, "RRPW op2");
    need_field(pos, 0, 31, "bit position");
    need_field(width, 0, 31, "bit-field width");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)s2 << 12) |
           ((unsigned long)width << 16) | ((unsigned long)op2 << 21) |
           ((unsigned long)pos << 23);
}

unsigned long tc_enc_rcpw(int op1, int op2, int d, int s1, unsigned c4,
                          int pos, int width)
{
    need_reg(s1);
    need_field(c4, 0, 15, "RCPW const4");
    need_field(op2, 0, 3, "RCPW op2");
    need_field(pos, 0, 31, "bit position");
    need_field(width, 0, 31, "bit-field width");
    return op_d(op1, d) | ((unsigned long)s1 << 8) | ((unsigned long)c4 << 12) |
           ((unsigned long)width << 16) | ((unsigned long)op2 << 21) |
           ((unsigned long)pos << 23);
}

/* BOL: the 16-bit offset is scattered -- bits 5:0 at 21:16, 9:6 at 31:28,
 * 15:10 at 27:22. */
unsigned long tc_enc_bol(int op1, int s1d, int s2, unsigned off16)
{
    need_field(op1, 0, 255, "op1");
    need_reg(s1d); need_reg(s2);
    need_field(off16, 0, 0xffff, "BOL offset");
    return (unsigned long)op1 | ((unsigned long)s1d << 8) |
           ((unsigned long)s2 << 12) | ((unsigned long)(off16 & 0x3f) << 16) |
           ((unsigned long)((off16 >> 10) & 0x3f) << 22) |
           ((unsigned long)((off16 >> 6) & 0xf) << 28);
}

/* BO: the 10-bit offset in two pieces -- bits 5:0 at 21:16, 9:6 at
 * 31:28 -- and op2 at 27:22. */
unsigned long tc_enc_bo(int op1, int op2, int s1d, int s2, unsigned off10)
{
    need_field(op1, 0, 255, "op1");
    need_reg(s1d); need_reg(s2);
    need_field(op2, 0, 63, "BO op2");
    need_field(off10, 0, 0x3ff, "BO offset");
    return (unsigned long)op1 | ((unsigned long)s1d << 8) |
           ((unsigned long)s2 << 12) | ((unsigned long)(off10 & 0x3f) << 16) |
           ((unsigned long)op2 << 22) |
           ((unsigned long)((off10 >> 6) & 0xf) << 28);
}

unsigned long tc_enc_brr(int op1, int op2, int s1, int s2, unsigned disp15)
{
    need_field(op1, 0, 255, "op1");
    need_reg(s1); need_reg(s2);
    need_field(op2, 0, 1, "BRR op2");
    need_field(disp15, 0, 0x7fff, "branch displacement");
    return (unsigned long)op1 | ((unsigned long)s1 << 8) |
           ((unsigned long)s2 << 12) | ((unsigned long)disp15 << 16) |
           ((unsigned long)op2 << 31);
}

unsigned long tc_enc_brc(int op1, int op2, int s1, unsigned c4,
                         unsigned disp15)
{
    need_field(c4, 0, 15, "BRC const4");
    return tc_enc_brr(op1, op2, s1, (int)c4, disp15);
}

/* B: the 24-bit displacement's low 16 bits at 31:16, its high 8 at 15:8. */
unsigned long tc_enc_b(int op1, unsigned long disp24)
{
    need_field(op1, 0, 255, "op1");
    need_field((long long)disp24, 0, 0xffffff, "24-bit displacement");
    return (unsigned long)op1 | (((disp24 >> 16) & 0xff) << 8) |
           ((disp24 & 0xffff) << 16);
}

unsigned long tc_enc_sys(int op1, int op2, int s1d)
{
    need_field(op1, 0, 255, "op1");
    need_reg(s1d);
    need_field(op2, 0, 63, "SYS op2");
    return (unsigned long)op1 | ((unsigned long)s1d << 8) |
           ((unsigned long)op2 << 22);
}

void tc_w(struct code *c, unsigned long w)
{
    if (!(w & 1))
        internal_error("tricore: a 32-bit word 0x%08lx without bit 0 set", w);
    code_u32(c, w & 0xffffffffUL);
}

void tc_h(struct code *c, unsigned h)
{
    if (h & 1)
        internal_error("tricore: a 16-bit halfword 0x%04x with bit 0 set", h);
    code_u16(c, h & 0xffffu);
}

/* ---- the 16-bit forms --------------------------------------------------- */

/* The two-operand halfword formats: op1 in bits 7:0, the first register
 * field (s1/d) in 11:8 and the second (s2, or a 4-bit constant) in 15:12
 * -- SRR, SRC, SLR and SSR are all this one layout. */
unsigned tc_enc16(int op1, int r1, unsigned r2)
{
    need_field(op1, 0, 255, "op1");
    if (op1 & 1)
        internal_error("tricore: a 16-bit op1 0x%02x with bit 0 set", op1);
    need_reg(r1);
    need_field(r2, 0, 15, "16-bit second field");
    return (unsigned)op1 | ((unsigned)r1 << 8) | (r2 << 12);
}

enum {
    OPC16_MOV = 0x02,        /* SRR  D[a] = D[b] */
    OPC16_MOV_K4 = 0x82,     /* SRC  D[a] = sext(const4) */
    OPC16_MOV_A = 0x60,      /* SRR  A[a] = D[b] */
    OPC16_MOV_D = 0x80,      /* SRR  D[a] = A[b] */
    OPC16_MOV_AA = 0x40,     /* SRR  A[a] = A[b] */
    OPC16_LD_W = 0x54, OPC16_LD_BU = 0x14, OPC16_LD_H = 0x94,
    OPC16_LD_A = 0xd4,       /* SLR  D[c]/A[c] = *A[b] */
    OPC16_ST_W = 0x74, OPC16_ST_B = 0x34, OPC16_ST_H = 0xb4,
    OPC16_ST_A = 0xf4        /* SSR  *A[b] = D[a]/A[a] */
};

/* Whether the emitters below may choose a 16-bit form: off for the
 * referee's 32-bit sweep and EmbLD's fixed-size stub, on while the code
 * generator runs. */
static int g_short;
void tc_set_short(int on) { g_short = on; }

void tc_mov16(struct code *c, int da, int db)
{
    tc_h(c, tc_enc16(OPC16_MOV, da, (unsigned)db));
}

void tc_mov_k4(struct code *c, int da, long long k)
{
    tc_h(c, tc_enc16(OPC16_MOV_K4, da, field(k, 4, 1, "mov const4")));
}

void tc_mov_a16(struct code *c, int aa, int db)
{
    tc_h(c, tc_enc16(OPC16_MOV_A, aa, (unsigned)db));
}

void tc_mov_d16(struct code *c, int da, int ab)
{
    tc_h(c, tc_enc16(OPC16_MOV_D, da, (unsigned)ab));
}

void tc_mov_aa16(struct code *c, int aa, int ab)
{
    tc_h(c, tc_enc16(OPC16_MOV_AA, aa, (unsigned)ab));
}

int tc_load16_ok(int size, int sign)
{
    return (size == 4) || (size == 1 && !sign) || (size == 2 && sign);
}

void tc_load16(struct code *c, int dc, int ab, int size, int sign)
{
    int op = size == 4 ? OPC16_LD_W : size == 1 ? OPC16_LD_BU : OPC16_LD_H;
    if (!tc_load16_ok(size, sign))
        internal_error("tricore: no 16-bit load of %d bytes, %s", size,
                       sign ? "signed" : "unsigned");
    tc_h(c, tc_enc16(op, dc, (unsigned)ab));
}

void tc_store16(struct code *c, int da, int ab, int size)
{
    int op = size == 4 ? OPC16_ST_W : size == 1 ? OPC16_ST_B : OPC16_ST_H;
    need_field(size == 3 ? 0 : size, 1, 4, "16-bit store size");
    tc_h(c, tc_enc16(op, da, (unsigned)ab));
}

void tc_ld_a16(struct code *c, int ac, int ab)
{
    tc_h(c, tc_enc16(OPC16_LD_A, ac, (unsigned)ab));
}

void tc_st_a16(struct code *c, int aa, int ab)
{
    tc_h(c, tc_enc16(OPC16_ST_A, aa, (unsigned)ab));
}

/* ---- moves and constants ---------------------------------------------- */

void tc_mov(struct code *c, int dc, int db)
{
    if (g_short) {
        tc_mov16(c, dc, db);
        return;
    }
    tc_w(c, tc_enc_rr(OPC_RR_ACCUMULATOR, RR_MOV, dc, 0, db, 0));
}

void tc_mov_a(struct code *c, int ac, int db)
{
    if (g_short) {
        tc_mov_a16(c, ac, db);
        return;
    }
    tc_w(c, tc_enc_rr(OPC_RR_ADDRESS, RR_MOV_A, ac, 0, db, 0));
}

void tc_mov_d(struct code *c, int dc, int ab)
{
    if (g_short) {
        tc_mov_d16(c, dc, ab);
        return;
    }
    tc_w(c, tc_enc_rr(OPC_RR_ADDRESS, RR_MOV_D, dc, 0, ab, 0));
}

void tc_mov_aa(struct code *c, int ac, int ab)
{
    if (g_short) {
        tc_mov_aa16(c, ac, ab);
        return;
    }
    tc_w(c, tc_enc_rr(OPC_RR_ADDRESS, RR_MOV_AA, ac, 0, ab, 0));
}

void tc_mov_imm(struct code *c, int dc, long long v)
{
    if (g_short && v >= -8 && v <= 7) {
        tc_mov_k4(c, dc, v);
        return;
    }
    tc_w(c, tc_enc_rlc(OPC_RLC_MOV, dc, 0, field(v, 16, 1, "mov")));
}

void tc_mov_u(struct code *c, int dc, long long v)
{
    tc_w(c, tc_enc_rlc(OPC_RLC_MOV_U, dc, 0, field(v, 16, 0, "mov.u")));
}

void tc_movh(struct code *c, int dc, unsigned v16)
{
    tc_w(c, tc_enc_rlc(OPC_RLC_MOVH, dc, 0, field(v16, 16, 0, "movh")));
}

void tc_movh_a(struct code *c, int ac, unsigned v16)
{
    tc_w(c, tc_enc_rlc(OPC_RLC_MOVH_A, ac, 0, field(v16, 16, 0, "movh.a")));
}

unsigned tc_hi_adj(unsigned long v)
{
    return (unsigned)(((v & 0xffffffffUL) + 0x8000UL) >> 16) & 0xffffu;
}

static long long lo16s(unsigned long v)
{
    return (long long)(short)(unsigned short)(v & 0xffff);
}

void tc_li(struct code *c, int dc, long long v)
{
    unsigned long u = (unsigned long)v & 0xffffffffUL;
    long long s = (long long)(int)(unsigned int)u;
    if (s >= -32768 && s <= 32767) {
        tc_mov_imm(c, dc, s);
        return;
    }
    if (u <= 0xffff) {
        tc_mov_u(c, dc, (long long)u);
        return;
    }
    tc_movh(c, dc, tc_hi_adj(u));
    if (lo16s(u))
        tc_addi(c, dc, dc, lo16s(u));
}

int tc_li_len(long long v)
{
    unsigned long u = (unsigned long)v & 0xffffffffUL;
    long long s = (long long)(int)(unsigned int)u;
    if ((s >= -32768 && s <= 32767) || u <= 0xffff)
        return 4;
    return lo16s(u) ? 8 : 4;
}

void tc_li_a(struct code *c, int ac, unsigned long v)
{
    tc_movh_a(c, ac, tc_hi_adj(v));
    if (lo16s(v))
        tc_lea(c, ac, ac, lo16s(v));
}

void tc_addi(struct code *c, int dc, int da, long long v)
{
    tc_w(c, tc_enc_rlc(OPC_RLC_ADDI, dc, da, field(v, 16, 1, "addi")));
}

void tc_addih(struct code *c, int dc, int da, unsigned v16)
{
    tc_w(c, tc_enc_rlc(OPC_RLC_ADDIH, dc, da, field(v16, 16, 0, "addih")));
}

void tc_addih_a(struct code *c, int ac, int aa, unsigned v16)
{
    tc_w(c, tc_enc_rlc(OPC_RLC_ADDIH_A, ac, aa,
                       field(v16, 16, 0, "addih.a")));
}

void tc_lea(struct code *c, int ac, int ab, long long off)
{
    tc_w(c, tc_enc_bol(OPC_BOL_LEA, ac, ab, field(off, 16, 1, "lea")));
}

void tc_add_a(struct code *c, int ac, int aa, int ab)
{
    tc_w(c, tc_enc_rr(OPC_RR_ADDRESS, RR_ADD_A, ac, aa, ab, 0));
}

void tc_sub_a(struct code *c, int ac, int aa, int ab)
{
    tc_w(c, tc_enc_rr(OPC_RR_ADDRESS, RR_SUB_A, ac, aa, ab, 0));
}

void tc_addsc_a(struct code *c, int ac, int ab, int da, int n)
{
    tc_w(c, tc_enc_rr(OPC_RR_ADDRESS, RR_ADDSC_A, ac, da, ab, n));
}

/* ---- arithmetic and logic --------------------------------------------- */

/* Each operation's register form and its immediate form: the op1/op2 of
 * each (0 op1: none), and how the RC form extends its const9 -- 's'
 * signed, 'u' unsigned, 'h' a shift count (signed, -32..31). */
static const struct alu_form {
    int rr_op1, rr_op2;
    int rc_op1, rc_op2;
    char ext;
} alu_forms[TC_NALU] = {
    [TC_ADD]  = { 0x0b, 0x00, 0x8b, 0x00, 's' },
    [TC_SUB]  = { 0x0b, 0x08, 0,    0,    0   },
    [TC_ADDX] = { 0x0b, 0x04, 0x8b, 0x04, 's' },
    [TC_ADDC] = { 0x0b, 0x05, 0x8b, 0x05, 's' },
    [TC_SUBX] = { 0x0b, 0x0c, 0,    0,    0   },
    [TC_SUBC] = { 0x0b, 0x0d, 0,    0,    0   },
    [TC_EQ]   = { 0x0b, 0x10, 0x8b, 0x10, 's' },
    [TC_NE]   = { 0x0b, 0x11, 0x8b, 0x11, 's' },
    [TC_LT]   = { 0x0b, 0x12, 0x8b, 0x12, 's' },
    [TC_LTU]  = { 0x0b, 0x13, 0x8b, 0x13, 'u' },
    [TC_GE]   = { 0x0b, 0x14, 0x8b, 0x14, 's' },
    [TC_GEU]  = { 0x0b, 0x15, 0x8b, 0x15, 'u' },
    [TC_MIN]  = { 0x0b, 0x18, 0x8b, 0x18, 's' },
    [TC_MINU] = { 0x0b, 0x19, 0x8b, 0x19, 'u' },
    [TC_MAX]  = { 0x0b, 0x1a, 0x8b, 0x1a, 's' },
    [TC_MAXU] = { 0x0b, 0x1b, 0x8b, 0x1b, 'u' },
    [TC_AND]  = { 0x0f, 0x08, 0x8f, 0x08, 'u' },
    [TC_OR]   = { 0x0f, 0x0a, 0x8f, 0x0a, 'u' },
    [TC_XOR]  = { 0x0f, 0x0c, 0x8f, 0x0c, 'u' },
    [TC_NOR]  = { 0x0f, 0x0b, 0x8f, 0x0b, 'u' },
    [TC_ANDN] = { 0x0f, 0x0e, 0x8f, 0x0e, 'u' },
    [TC_ORN]  = { 0x0f, 0x0f, 0x8f, 0x0f, 'u' },
    [TC_NAND] = { 0x0f, 0x09, 0x8f, 0x09, 'u' },
    [TC_XNOR] = { 0x0f, 0x0d, 0x8f, 0x0d, 'u' },
    [TC_SH]   = { 0x0f, 0x00, 0x8f, 0x00, 'h' },
    [TC_SHA]  = { 0x0f, 0x01, 0x8f, 0x01, 'h' },
    [TC_MUL]  = { 0,    0,    0x53, 0x01, 's' },   /* RR form: RR2, below */
    [TC_RSUB] = { 0,    0,    0x8b, 0x08, 's' },
};

void tc_alu(struct code *c, int op, int dc, int da, int db)
{
    need_field(op, 0, TC_NALU - 1, "ALU operation");
    if (op == TC_MUL) {
        tc_w(c, tc_enc_rr2(OPC_RR2_MUL, RR2_MUL32, dc, da, db));
        return;
    }
    if (!alu_forms[op].rr_op1)
        internal_error("tricore: ALU operation %d has no register form", op);
    tc_w(c, tc_enc_rr(alu_forms[op].rr_op1, alu_forms[op].rr_op2, dc, da, db,
                      0));
}

int tc_alu_imm_ok(int op, long long imm)
{
    if (op < 0 || op >= TC_NALU || !alu_forms[op].rc_op1)
        return 0;
    switch (alu_forms[op].ext) {
    case 's': return tc_fits(imm, 9, 1);
    case 'u': return tc_fits(imm, 9, 0);
    default:  return imm >= -32 && imm <= 31;
    }
}

void tc_alu_imm(struct code *c, int op, int dc, int da, long long imm)
{
    unsigned k;
    need_field(op, 0, TC_NALU - 1, "ALU operation");
    if (!alu_forms[op].rc_op1)
        internal_error("tricore: ALU operation %d has no immediate form", op);
    if (alu_forms[op].ext == 'h') {
        need_field(imm, -32, 31, "shift count");
        k = (unsigned)(imm & 0x3f);
    } else {
        k = field(imm, 9, alu_forms[op].ext == 's', "const9");
    }
    tc_w(c, tc_enc_rc(alu_forms[op].rc_op1, alu_forms[op].rc_op2, dc, da, k));
}

void tc_mul64(struct code *c, int ec, int da, int db, int sign)
{
    need_even(ec);
    tc_w(c, tc_enc_rr2(OPC_RR2_MUL, sign ? RR2_MUL64 : RR2_MUL64_U, ec, da,
                       db));
}

void tc_madd(struct code *c, int dc, int dd, int da, int db)
{
    tc_w(c, tc_enc_rrr2(OPC_RRR2_MADD, RRR2_MADD32, dc, da, db, dd));
}

void tc_div(struct code *c, int ec, int da, int db, int sign)
{
    need_even(ec);
    tc_w(c, tc_enc_rr(OPC_RR_DIVIDE, sign ? RR_DIV : RR_DIV_U, ec, da, db, 0));
}

void tc_clz(struct code *c, int dc, int da)
{
    tc_w(c, tc_enc_rr(OPC_RR_LOGICAL_SHIFT, RR_CLZ, dc, da, 0, 0));
}

static void need_bits(int pos, int width)
{
    need_field(pos, 0, 31, "bit position");
    need_field(width, 1, 31, "bit-field width");
    if (pos + width > 32)
        internal_error("tricore: a %d-bit field at bit %d runs past bit 31",
                       width, pos);
}

void tc_extr(struct code *c, int dc, int da, int pos, int width, int sign)
{
    need_bits(pos, width);
    tc_w(c, tc_enc_rrpw(OPC_RRPW_EXTRACT, sign ? RRPW_EXTR : RRPW_EXTR_U, dc,
                        da, 0, pos, width));
}

void tc_insert(struct code *c, int dc, int da, int db, int pos, int width)
{
    need_bits(pos, width);
    tc_w(c, tc_enc_rrpw(OPC_RRPW_EXTRACT, RRPW_INSERT, dc, da, db, pos,
                        width));
}

void tc_insert_imm(struct code *c, int dc, int da, unsigned k4, int pos,
                   int width)
{
    need_bits(pos, width);
    tc_w(c, tc_enc_rcpw(OPC_RCPW_INSERT, RRPW_INSERT, dc, da, k4, pos,
                        width));
}

void tc_dextr(struct code *c, int dc, int dhi, int dlo, int pos)
{
    tc_w(c, tc_enc_rrpw(OPC_RRPW_DEXTR, 0, dc, dhi, dlo, pos, 0));
}

void tc_dextr_r(struct code *c, int dc, int dhi, int dlo, int dp)
{
    tc_w(c, tc_enc_rrrr(OPC_RRRR_EXTRACT, RRRR_DEXTR, dc, dhi, dlo, dp));
}

void tc_sel(struct code *c, int dc, int dcond, int dt, int df)
{
    tc_w(c, tc_enc_rrr(OPC_RRR_COND_SELECT, RRR_SEL, dc, dt, df, dcond, 0));
}

void tc_seln(struct code *c, int dc, int dcond, int dt, int df)
{
    tc_w(c, tc_enc_rrr(OPC_RRR_COND_SELECT, RRR_SELN, dc, dt, df, dcond, 0));
}

/* ---- memory ----------------------------------------------------------- */

void tc_load(struct code *c, int dt, int ab, long long off, int size,
             int sign)
{
    if (g_short && off == 0 && tc_load16_ok(size, sign)) {
        tc_load16(c, dt, ab, size, sign);
        return;
    }
    int op = 0;
    switch (size) {
    case 1: op = sign ? OPC_BOL_LD_B : OPC_BOL_LD_BU; break;
    case 2: op = sign ? OPC_BOL_LD_H : OPC_BOL_LD_HU; break;
    case 4: op = OPC_BOL_LD_W; break;
    default: internal_error("tricore: a %d-byte load", size);
    }
    tc_w(c, tc_enc_bol(op, dt, ab, field(off, 16, 1, "load offset")));
}

void tc_store(struct code *c, int dt, int ab, long long off, int size)
{
    if (g_short && off == 0 && (size == 1 || size == 2 || size == 4)) {
        tc_store16(c, dt, ab, size);
        return;
    }
    int op = 0;
    switch (size) {
    case 1: op = OPC_BOL_ST_B; break;
    case 2: op = OPC_BOL_ST_H; break;
    case 4: op = OPC_BOL_ST_W; break;
    default: internal_error("tricore: a %d-byte store", size);
    }
    tc_w(c, tc_enc_bol(op, dt, ab, field(off, 16, 1, "store offset")));
}

void tc_ld_a(struct code *c, int at, int ab, long long off)
{
    if (g_short && off == 0) {
        tc_ld_a16(c, at, ab);
        return;
    }
    tc_w(c, tc_enc_bol(OPC_BOL_LD_A, at, ab, field(off, 16, 1, "ld.a offset")));
}

void tc_st_a(struct code *c, int at, int ab, long long off)
{
    if (g_short && off == 0) {
        tc_st_a16(c, at, ab);
        return;
    }
    tc_w(c, tc_enc_bol(OPC_BOL_ST_A, at, ab, field(off, 16, 1, "st.a offset")));
}

void tc_swap_w(struct code *c, int da, int ab, long long off)
{
    tc_w(c, tc_enc_bo(OPC_BO_STCTX, BO_SWAP_W, da, ab,
                      field(off, 10, 1, "swap.w offset")));
}

void tc_cmpswap_w(struct code *c, int ea, int ab, long long off)
{
    need_even(ea);
    tc_w(c, tc_enc_bo(OPC_BO_STCTX, BO_CMPSWAP_W, ea, ab,
                      field(off, 10, 1, "cmpswap.w offset")));
}

/* ---- control flow ----------------------------------------------------- */

/* Each condition's BRR op1/op2, its BRC op1 (0: none) and how BRC extends
 * its constant. JZ_A and JNZ_A ignore s2. */
static const struct cond_form {
    int op1, op2, brc_op1;
    int ksign;
} cond_forms[] = {
    [TC_JEQ]   = { OPC_BRR_JEQ,   0, OPC_BRC_JEQ, 1 },
    [TC_JNE]   = { OPC_BRR_JEQ,   1, OPC_BRC_JEQ, 1 },
    [TC_JLT]   = { OPC_BRR_JLT,   0, OPC_BRC_JLT, 1 },
    [TC_JLTU]  = { OPC_BRR_JLT,   1, OPC_BRC_JLT, 0 },
    [TC_JGE]   = { OPC_BRR_JGE,   0, OPC_BRC_JGE, 1 },
    [TC_JGEU]  = { OPC_BRR_JGE,   1, OPC_BRC_JGE, 0 },
    [TC_JEQ_A] = { OPC_BRR_JEQ_A, 0, 0, 0 },
    [TC_JNE_A] = { OPC_BRR_JEQ_A, 1, 0, 0 },
    [TC_JZ_A]  = { OPC_BRR_JZ_A,  0, 0, 0 },
    [TC_JNZ_A] = { OPC_BRR_JZ_A,  1, 0, 0 },
};

static unsigned disp15(long off)
{
    if (off & 1)
        internal_error("tricore: an odd branch displacement %ld", off);
    need_field(off, -32768, 32766, "branch displacement");
    return (unsigned)((off / 2) & 0x7fff);
}

static unsigned long disp24(long off)
{
    if (off & 1)
        internal_error("tricore: an odd jump displacement %ld", off);
    need_field(off, -16777216L, 16777214L, "jump displacement");
    return (unsigned long)(off / 2) & 0xffffffUL;
}

unsigned long tc_enc_jcc(int cond, int s1, int s2, long off)
{
    need_field(cond, TC_JEQ, TC_JNZ_A, "branch condition");
    if (cond == TC_JZ_A || cond == TC_JNZ_A)
        s2 = 0;
    return tc_enc_brr(cond_forms[cond].op1, cond_forms[cond].op2, s1, s2,
                      disp15(off));
}

int tc_jcci_ok(int cond, long long k4)
{
    if (cond < TC_JEQ || cond > TC_JGEU)
        return 0;
    return tc_fits(k4, 4, cond_forms[cond].ksign);
}

unsigned long tc_enc_jcci(int cond, int s1, long long k4, long off)
{
    need_field(cond, TC_JEQ, TC_JGEU, "branch-on-constant condition");
    return tc_enc_brc(cond_forms[cond].brc_op1, cond_forms[cond].op2, s1,
                      field(k4, 4, cond_forms[cond].ksign, "branch constant"),
                      disp15(off));
}

int tc_jcc_placeholder(struct code *c, int cond, int s1, int s2)
{
    int at = c->len;
    tc_w(c, tc_enc_jcc(cond, s1, s2, 0));
    return at;
}

int tc_jcci_placeholder(struct code *c, int cond, int s1, long long k4)
{
    int at = c->len;
    tc_w(c, tc_enc_jcci(cond, s1, k4, 0));
    return at;
}

unsigned long tc_enc_j(long off)    { return tc_enc_b(OPC_B_J, disp24(off)); }
unsigned long tc_enc_call(long off) { return tc_enc_b(OPC_B_CALL, disp24(off)); }
unsigned long tc_enc_jl(long off)   { return tc_enc_b(OPC_B_JL, disp24(off)); }

int tc_j_placeholder(struct code *c)
{
    int at = c->len;
    tc_w(c, tc_enc_j(0));
    return at;
}

int tc_br_reaches(long at, long target)
{
    long d = target - at;
    return !(d & 1) && d >= -32768 && d <= 32766;
}

int tc_patch(struct code *c, int at, int target)
{
    unsigned long w = (unsigned long)c->p[at] |
                      ((unsigned long)c->p[at + 1] << 8) |
                      ((unsigned long)c->p[at + 2] << 16) |
                      ((unsigned long)c->p[at + 3] << 24);
    long d = (long)target - (long)at;
    int op1 = (int)(w & 0xff);
    if (op1 == OPC_B_J || op1 == OPC_B_CALL || op1 == OPC_B_JL) {
        if ((d & 1) || d < -16777216L || d > 16777214L)
            return 0;
        code_patch32(c, at, tc_enc_b(op1, (unsigned long)(d / 2) & 0xffffffUL));
        return 1;
    }
    switch (op1) {
    case OPC_BRR_JEQ: case OPC_BRR_JLT: case OPC_BRR_JGE:
    case OPC_BRR_JEQ_A: case OPC_BRR_JZ_A:
    case OPC_BRC_JEQ: case OPC_BRC_JLT: case OPC_BRC_JGE:
        break;
    default:
        internal_error("tricore: patching 0x%08lx, which is not a branch", w);
    }
    if (!tc_br_reaches(at, target))
        return 0;
    w = (w & ~(0x7fffUL << 16)) | ((unsigned long)disp15(d) << 16);
    code_patch32(c, at, w);
    return 1;
}

void tc_call0(struct code *c) { tc_w(c, tc_enc_call(0)); }
void tc_j0(struct code *c)    { tc_w(c, tc_enc_j(0)); }

void tc_ji(struct code *c, int aa)
{
    tc_w(c, tc_enc_rr(OPC_RR_IDIRECT, RR_JI, 0, aa, 0, 0));
}

void tc_jli(struct code *c, int aa)
{
    tc_w(c, tc_enc_rr(OPC_RR_IDIRECT, RR_JLI, 0, aa, 0, 0));
}

void tc_calli(struct code *c, int aa)
{
    tc_w(c, tc_enc_rr(OPC_RR_IDIRECT, RR_CALLI, 0, aa, 0, 0));
}

void tc_ret(struct code *c)
{
    if (g_short) {
        tc_ret16(c);
        return;
    }
    tc_w(c, tc_enc_sys(OPC_SYS, SYS_RET, 0));
}
void tc_ret16(struct code *c) { tc_h(c, 0x9000); }

/* ---- the system ------------------------------------------------------- */

void tc_nop(struct code *c)     { tc_w(c, tc_enc_sys(OPC_SYS, SYS_NOP, 0)); }
void tc_nop16(struct code *c)   { tc_h(c, 0x0000); }
void tc_debug(struct code *c)   { tc_w(c, tc_enc_sys(OPC_SYS, SYS_DEBUG, 0)); }
void tc_isync(struct code *c)   { tc_w(c, tc_enc_sys(OPC_SYS, SYS_ISYNC, 0)); }
void tc_dsync(struct code *c)   { tc_w(c, tc_enc_sys(OPC_SYS, SYS_DSYNC, 0)); }
void tc_svlcx(struct code *c)   { tc_w(c, tc_enc_sys(OPC_SYS, SYS_SVLCX, 0)); }
void tc_rslcx(struct code *c)   { tc_w(c, tc_enc_sys(OPC_SYS, SYS_RSLCX, 0)); }
void tc_enable(struct code *c)  { tc_w(c, tc_enc_sys(OPC_SYS, SYS_ENABLE, 0)); }
void tc_disable(struct code *c) { tc_w(c, tc_enc_sys(OPC_SYS, SYS_DISABLE, 0)); }
void tc_rfe(struct code *c)     { tc_w(c, tc_enc_sys(OPC_SYS, SYS_RFE, 0)); }
void tc_illegal(struct code *c)
{
    tc_w(c, tc_enc_rr(OPC_RR_ADDRESS, 0xff, 0, 0, 0, 0));
}

/* The field is const9, but the trap's identification number is its low
 * eight bits (QEMU agrees: SYSCALL 511 traps with TIN 255), so a number
 * above 255 is refused rather than silently aliased. */
void tc_syscall(struct code *c, unsigned k8)
{
    tc_w(c, tc_enc_rc(OPC_RC_SERVICE, RC_SYSCALL, 0, 0,
                      field(k8, 8, 0, "syscall number")));
}

void tc_mtcr(struct code *c, unsigned csfr, int da)
{
    tc_w(c, tc_enc_rlc(OPC_RLC_MTCR, 0, da, field(csfr, 16, 0, "CSFR")));
}

void tc_mfcr(struct code *c, int dc, unsigned csfr)
{
    tc_w(c, tc_enc_rlc(OPC_RLC_MFCR, dc, 0, field(csfr, 16, 0, "CSFR")));
}
