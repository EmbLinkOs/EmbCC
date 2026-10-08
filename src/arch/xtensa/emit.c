/* Xtensa instruction encoding. See emit.h for the shape; the field names
 * are the Xtensa ISA Reference Manual's (chapter 7, "Instruction
 * Formats and Opcodes"). */
#include "emit.h"

#include "../../driver/util.h"

static const char *const reg_names[16] = {
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7",
    "a8", "a9", "a10", "a11", "a12", "a13", "a14", "a15"
};

const char *xt_reg_name(int r)
{
    return r >= 0 && r < 16 ? reg_names[r] : "?";
}

static void need_reg(int r)
{
    if (r < 0 || r > 15)
        internal_error("xtensa: register %d is not a0-a15", r);
}

static void need_field(long long v, long long lo, long long hi,
                       const char *what)
{
    if (v < lo || v > hi)
        internal_error("xtensa: %s %lld is outside %lld..%lld", what, v, lo,
                       hi);
}

static void need4(long long v, unsigned bits, const char *what)
{
    need_field(v, 0, (1LL << bits) - 1, what);
}

/* ---- the formats --------------------------------------------------------- */

unsigned long xt_enc_rrr(int op0, int op1, int op2, int r, int s, int t)
{
    need4(op0, 4, "op0"); need4(op1, 4, "op1"); need4(op2, 4, "op2");
    need4(r, 4, "r field"); need4(s, 4, "s field"); need4(t, 4, "t field");
    return (unsigned long)op0 | ((unsigned long)t << 4) |
           ((unsigned long)s << 8) | ((unsigned long)r << 12) |
           ((unsigned long)op1 << 16) | ((unsigned long)op2 << 20);
}

unsigned long xt_enc_rri8(int op0, int r, int s, int t, unsigned imm8)
{
    need4(op0, 4, "op0"); need4(r, 4, "r field"); need4(s, 4, "s field");
    need4(t, 4, "t field"); need4(imm8, 8, "imm8 field");
    return (unsigned long)op0 | ((unsigned long)t << 4) |
           ((unsigned long)s << 8) | ((unsigned long)r << 12) |
           ((unsigned long)imm8 << 16);
}

unsigned long xt_enc_ri16(int op0, int t, unsigned imm16)
{
    need4(op0, 4, "op0"); need4(t, 4, "t field");
    need4(imm16, 16, "imm16 field");
    return (unsigned long)op0 | ((unsigned long)t << 4) |
           ((unsigned long)imm16 << 8);
}

unsigned long xt_enc_call(int op0, int n, unsigned long off18)
{
    need4(op0, 4, "op0"); need4(n, 2, "n field");
    need4((long long)off18, 18, "offset18 field");
    return (unsigned long)op0 | ((unsigned long)n << 4) | (off18 << 6);
}

unsigned long xt_enc_bri8(int op0, int n, int m, int s, int r, unsigned imm8)
{
    need4(op0, 4, "op0"); need4(n, 2, "n field"); need4(m, 2, "m field");
    need4(s, 4, "s field"); need4(r, 4, "r field");
    need4(imm8, 8, "imm8 field");
    return (unsigned long)op0 | ((unsigned long)n << 4) |
           ((unsigned long)m << 6) | ((unsigned long)s << 8) |
           ((unsigned long)r << 12) | ((unsigned long)imm8 << 16);
}

unsigned long xt_enc_bri12(int op0, int n, int m, int s, unsigned imm12)
{
    need4(op0, 4, "op0"); need4(n, 2, "n field"); need4(m, 2, "m field");
    need4(s, 4, "s field"); need4(imm12, 12, "imm12 field");
    return (unsigned long)op0 | ((unsigned long)n << 4) |
           ((unsigned long)m << 6) | ((unsigned long)s << 8) |
           ((unsigned long)imm12 << 12);
}

void xt_w(struct code *c, unsigned long w)
{
    code_byte(c, (int)(w & 0xff));
    code_byte(c, (int)((w >> 8) & 0xff));
    code_byte(c, (int)((w >> 16) & 0xff));
}

unsigned long xt_get(const struct code *c, int at)
{
    return (unsigned long)c->p[at] | ((unsigned long)c->p[at + 1] << 8) |
           ((unsigned long)c->p[at + 2] << 16);
}

void xt_put(struct code *c, int at, unsigned long w)
{
    c->p[at] = (unsigned char)(w & 0xff);
    c->p[at + 1] = (unsigned char)((w >> 8) & 0xff);
    c->p[at + 2] = (unsigned char)((w >> 16) & 0xff);
}

/* The opcode groups (op0), and the sub-opcodes the ISA manual names. */
enum { OP0_QRST = 0, OP0_L32R = 1, OP0_LSAI = 2, OP0_CALLN = 5, OP0_SI = 6,
       OP0_B = 7 };
enum { OP1_RST0 = 0, OP1_RST1 = 1, OP1_RST2 = 2, OP1_RST3 = 3,
       OP1_LSC4 = 9 };
/* LSAI's r field */
enum { LSAI_L8UI = 0, LSAI_L16UI = 1, LSAI_L32I = 2, LSAI_S8I = 4,
       LSAI_S16I = 5, LSAI_S32I = 6, LSAI_L16SI = 9, LSAI_MOVI = 10,
       LSAI_L32AI = 11, LSAI_ADDI = 12, LSAI_ADDMI = 13, LSAI_S32C1I = 14,
       LSAI_S32RI = 15 };

/* An RST0/ST0 instruction: op0 = op1 = op2 = 0, distinguished by r, s, t. */
static void st0(struct code *c, int r, int s, int t)
{
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 0, r, s, t));
}

/* ---- moves and constants ------------------------------------------------- */

void xt_mov(struct code *c, int d, int s)
{
    need_reg(d); need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 2, d, s, s));
}

int xt_movi_ok(long long v)
{
    return v >= -2048 && v <= 2047;
}

void xt_movi(struct code *c, int t, long imm)
{
    unsigned long f;
    need_reg(t);
    need_field(imm, -2048, 2047, "movi immediate");
    f = (unsigned long)imm & 0xfff;
    xt_w(c, xt_enc_rri8(OP0_LSAI, LSAI_MOVI, (int)(f >> 8), t,
                        (unsigned)(f & 0xff)));
}

/* The low 32 bits of v, sign-extended: what a register holds. */
static long long low32(long long v)
{
    return (long long)(int)(unsigned int)(unsigned long long)v;
}

/* How xt_li_inline builds v: `m` into the register with movi, then shifted
 * left by `k` (0 for none), then `add` added (0 for none). 0 when it
 * cannot. */
static int li_plan(long long v, long *m, int *k, long long *add)
{
    v = low32(v);
    if (xt_movi_ok(v)) {
        *m = (long)v; *k = 0; *add = 0;
        return 1;
    }
    /* m << k, m a 12-bit signed value */
    for (int s = 1; s < 32; s++) {
        long long q = v >> s;      /* arithmetic: v is a signed 32-bit value */
        if (q * (1LL << s) == v && xt_movi_ok(q)) {
            *m = (long)q; *k = s; *add = 0;
            return 1;
        }
    }
    /* m << k plus a small remainder that addi or addmi covers */
    for (int s = 8; s < 32; s++) {
        long long q = v >> s, rem;
        if (!xt_movi_ok(q))
            continue;
        rem = v - q * (1LL << s);
        if (rem >= -128 && rem <= 127) {
            *m = (long)q; *k = s; *add = rem;
            return 1;
        }
        if (xt_movi_ok(q + 1)) {
            rem = v - (q + 1) * (1LL << s);
            if (rem >= -128 && rem <= 127) {
                *m = (long)(q + 1); *k = s; *add = rem;
                return 1;
            }
        }
    }
    return 0;
}

int xt_li_inline_len(long long v)
{
    long m; int k; long long add;
    if (!li_plan(v, &m, &k, &add))
        return 0;
    return 3 + (k ? 3 : 0) + (add ? 3 : 0);
}

int xt_li_inline(struct code *c, int t, long long v)
{
    long m; int k; long long add;
    if (!li_plan(v, &m, &k, &add))
        return 0;
    xt_movi(c, t, m);
    if (k)
        xt_slli(c, t, t, k);
    if (add)
        xt_addi(c, t, t, (long)add);
    return 1;
}

/* ---- arithmetic and logic ------------------------------------------------ */

void xt_alu(struct code *c, int op, int r, int s, int t)
{
    int op1, op2;
    need_reg(r); need_reg(s); need_reg(t);
    switch (op) {
    case XT_AND:    op1 = OP1_RST0; op2 = 1; break;
    case XT_OR:     op1 = OP1_RST0; op2 = 2; break;
    case XT_XOR:    op1 = OP1_RST0; op2 = 3; break;
    case XT_ADD:    op1 = OP1_RST0; op2 = 8; break;
    case XT_ADDX2:  op1 = OP1_RST0; op2 = 9; break;
    case XT_ADDX4:  op1 = OP1_RST0; op2 = 10; break;
    case XT_ADDX8:  op1 = OP1_RST0; op2 = 11; break;
    case XT_SUB:    op1 = OP1_RST0; op2 = 12; break;
    case XT_SUBX2:  op1 = OP1_RST0; op2 = 13; break;
    case XT_SUBX4:  op1 = OP1_RST0; op2 = 14; break;
    case XT_SUBX8:  op1 = OP1_RST0; op2 = 15; break;
    case XT_SRC:    op1 = OP1_RST1; op2 = 8; break;
    case XT_MUL16U: op1 = OP1_RST1; op2 = 12; break;
    case XT_MUL16S: op1 = OP1_RST1; op2 = 13; break;
    case XT_MULL:   op1 = OP1_RST2; op2 = 8; break;
    case XT_QUOU:   op1 = OP1_RST2; op2 = 12; break;
    case XT_QUOS:   op1 = OP1_RST2; op2 = 13; break;
    case XT_REMU:   op1 = OP1_RST2; op2 = 14; break;
    case XT_REMS:   op1 = OP1_RST2; op2 = 15; break;
    case XT_MIN:    op1 = OP1_RST3; op2 = 4; break;
    case XT_MAX:    op1 = OP1_RST3; op2 = 5; break;
    case XT_MINU:   op1 = OP1_RST3; op2 = 6; break;
    case XT_MAXU:   op1 = OP1_RST3; op2 = 7; break;
    case XT_MOVEQZ: op1 = OP1_RST3; op2 = 8; break;
    case XT_MOVNEZ: op1 = OP1_RST3; op2 = 9; break;
    case XT_MOVLTZ: op1 = OP1_RST3; op2 = 10; break;
    case XT_MOVGEZ: op1 = OP1_RST3; op2 = 11; break;
    default:
        internal_error("xtensa: no ALU operation %d", op);
        return;
    }
    xt_w(c, xt_enc_rrr(OP0_QRST, op1, op2, r, s, t));
}

void xt_neg(struct code *c, int r, int t)
{
    need_reg(r); need_reg(t);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 6, r, 0, t));
}

void xt_abs(struct code *c, int r, int t)
{
    need_reg(r); need_reg(t);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 6, r, 1, t));
}

void xt_addi(struct code *c, int t, int s, long imm)
{
    need_reg(t); need_reg(s);
    need_field(imm, -128, 127, "addi immediate");
    xt_w(c, xt_enc_rri8(OP0_LSAI, LSAI_ADDI, s, t, (unsigned)imm & 0xff));
}

void xt_addmi(struct code *c, int t, int s, long imm)
{
    need_reg(t); need_reg(s);
    need_field(imm, -32768, 32512, "addmi immediate");
    if (imm & 255)
        internal_error("xtensa: addmi immediate %ld is not a multiple of "
                       "256", imm);
    xt_w(c, xt_enc_rri8(OP0_LSAI, LSAI_ADDMI, s, t,
                        (unsigned)(imm >> 8) & 0xff));
}

/* addi alone, addmi alone, or addmi then addi with the remainder in
 * -128..127 (the addmi part rounded to the nearest multiple of 256). */
int xt_addi_any_len(long long imm)
{
    long long hi;
    if (imm >= -128 && imm <= 127)
        return 3;
    hi = (imm + 128) & ~255LL;
    if (hi < -32768 || hi > 32512)
        return 0;
    return imm == hi ? 3 : 6;
}

int xt_addi_any(struct code *c, int t, int s, long long imm)
{
    long long hi;
    if (imm >= -128 && imm <= 127) {
        xt_addi(c, t, s, (long)imm);
        return 1;
    }
    hi = (imm + 128) & ~255LL;
    if (hi < -32768 || hi > 32512)
        return 0;
    xt_addmi(c, t, s, (long)hi);
    if (imm != hi)
        xt_addi(c, t, t, (long)(imm - hi));
    return 1;
}

/* ---- shifts ---------------------------------------------------------------- */

void xt_slli(struct code *c, int r, int s, int n)
{
    int f;
    need_reg(r); need_reg(s);
    need_field(n, 1, 31, "slli amount");
    f = 32 - n;                      /* the field holds 32 - n: 1..31 */
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST1, f >> 4, r, s, f & 15));
}

void xt_srli(struct code *c, int r, int t, int n)
{
    need_reg(r); need_reg(t);
    need_field(n, 0, 15, "srli amount");
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST1, 4, r, n, t));
}

void xt_srai(struct code *c, int r, int t, int n)
{
    need_reg(r); need_reg(t);
    need_field(n, 0, 31, "srai amount");
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST1, 2 | (n >> 4), r, n & 15, t));
}

void xt_extui(struct code *c, int r, int t, int shift, int bits)
{
    need_reg(r); need_reg(t);
    need_field(shift, 0, 31, "extui shift");
    need_field(bits, 1, 16, "extui width");
    xt_w(c, xt_enc_rrr(OP0_QRST, 4 | (shift >> 4), bits - 1, r, shift & 15,
                       t));
}

void xt_ssr(struct code *c, int s)
{
    need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 4, 0, s, 0));
}

void xt_ssl(struct code *c, int s)
{
    need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 4, 1, s, 0));
}

void xt_ssa8l(struct code *c, int s)
{
    need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 4, 2, s, 0));
}

void xt_ssai(struct code *c, int n)
{
    need_field(n, 0, 31, "ssai amount");
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 4, 4, n & 15, n >> 4));
}

void xt_sll(struct code *c, int r, int s)
{
    need_reg(r); need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST1, 10, r, s, 0));
}

void xt_srl(struct code *c, int r, int t)
{
    need_reg(r); need_reg(t);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST1, 9, r, 0, t));
}

void xt_sra(struct code *c, int r, int t)
{
    need_reg(r); need_reg(t);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST1, 11, r, 0, t));
}

void xt_sext(struct code *c, int r, int s, int b)
{
    need_reg(r); need_reg(s);
    need_field(b, 7, 22, "sext bit");
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST3, 2, r, s, b - 7));
}

void xt_clamps(struct code *c, int r, int s, int b)
{
    need_reg(r); need_reg(s);
    need_field(b, 7, 22, "clamps bit");
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST3, 3, r, s, b - 7));
}

void xt_nsa(struct code *c, int t, int s)
{
    need_reg(t); need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 4, 14, s, t));
}

void xt_nsau(struct code *c, int t, int s)
{
    need_reg(t); need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 4, 15, s, t));
}

/* ---- memory ------------------------------------------------------------------ */

int xt_mem_ok(long off, int size)
{
    if (size != 1 && size != 2 && size != 4)
        return 0;
    return off >= 0 && off % size == 0 && off / size <= 255;
}

static unsigned mem_field(long off, int size, const char *what)
{
    if (size != 1 && size != 2 && size != 4)
        internal_error("xtensa: a %d-byte %s", size, what);
    if (off < 0 || off % size != 0 || off / size > 255)
        internal_error("xtensa: %s offset %ld is not a multiple of %d in "
                       "0..%d", what, off, size, 255 * size);
    return (unsigned)(off / size);
}

void xt_load(struct code *c, int t, int base, long off, int size, int sign)
{
    unsigned f;
    int r;
    need_reg(t); need_reg(base);
    f = mem_field(off, size, "load");
    if (size == 1 && sign)
        internal_error("xtensa: there is no sign-extending byte load");
    r = size == 1 ? LSAI_L8UI : size == 2 ? (sign ? LSAI_L16SI : LSAI_L16UI)
                                          : LSAI_L32I;
    xt_w(c, xt_enc_rri8(OP0_LSAI, r, base, t, f));
}

void xt_store(struct code *c, int t, int base, long off, int size)
{
    unsigned f;
    need_reg(t); need_reg(base);
    f = mem_field(off, size, "store");
    xt_w(c, xt_enc_rri8(OP0_LSAI, size == 1 ? LSAI_S8I
                                  : size == 2 ? LSAI_S16I : LSAI_S32I,
                        base, t, f));
}

int xt_l32r_reaches(long at, long lit)
{
    long d = lit - ((at + 3) & ~3L);
    return (lit & 3) == 0 && d < 0 && d >= -262144;
}

unsigned long xt_enc_l32r(int t, long at, long lit)
{
    long d;
    need_reg(t);
    if (lit & 3)
        internal_error("xtensa: l32r literal at %ld is not 4-aligned", lit);
    d = lit - ((at + 3) & ~3L);
    if (d >= 0 || d < -262144)
        internal_error("xtensa: l32r at %ld cannot reach its literal at %ld "
                       "(it reaches only 4..262144 bytes back)", at, lit);
    /* the address is (at+3)&~3 plus (1^14 || imm16 || 00): imm16 is the
     * low 16 bits of the negative word displacement */
    return xt_enc_ri16(OP0_L32R, t, (unsigned)((unsigned long)(d >> 2) &
                                               0xffff));
}

void xt_l32r(struct code *c, int t, long lit)
{
    xt_w(c, xt_enc_l32r(t, c->len, lit));
}

void xt_l32ai(struct code *c, int t, int s, long off)
{
    need_reg(t); need_reg(s);
    xt_w(c, xt_enc_rri8(OP0_LSAI, LSAI_L32AI, s, t, mem_field(off, 4, "l32ai")));
}

void xt_s32ri(struct code *c, int t, int s, long off)
{
    need_reg(t); need_reg(s);
    xt_w(c, xt_enc_rri8(OP0_LSAI, LSAI_S32RI, s, t, mem_field(off, 4, "s32ri")));
}

void xt_s32c1i(struct code *c, int t, int s, long off)
{
    need_reg(t); need_reg(s);
    xt_w(c, xt_enc_rri8(OP0_LSAI, LSAI_S32C1I, s, t,
                        mem_field(off, 4, "s32c1i")));
}

/* l32e/s32e: r holds the offset / 4 as a negative 4-bit value, -16..-1. */
static int e_field(long off)
{
    need_field(off, -64, -4, "l32e/s32e offset");
    if (off & 3)
        internal_error("xtensa: l32e/s32e offset %ld is not a multiple of 4",
                       off);
    return (int)((off >> 2) & 15);
}

void xt_l32e(struct code *c, int t, int s, long off)
{
    need_reg(t); need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_LSC4, 0, e_field(off), s, t));
}

void xt_s32e(struct code *c, int t, int s, long off)
{
    need_reg(t); need_reg(s);
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_LSC4, 4, e_field(off), s, t));
}

/* ---- control flow ------------------------------------------------------------ */

int xt_b_reaches(long off) { return off >= -128 && off <= 127; }
int xt_bz_reaches(long off) { return off >= -2048 && off <= 2047; }
int xt_j_reaches(long off) { return off >= -131072 && off <= 131071; }

unsigned long xt_enc_b(int cond, int s, int t, long off)
{
    static const int rfield[] = {
        /* XT_BNONE */ 0, /* XT_BEQ */ 1, /* XT_BLT */ 2, /* XT_BLTU */ 3,
        /* XT_BALL */ 4, /* XT_BBC */ 5, /* XT_BANY */ 8, /* XT_BNE */ 9,
        /* XT_BGE */ 10, /* XT_BGEU */ 11, /* XT_BNALL */ 12, /* XT_BBS */ 13
    };
    need_reg(s); need_reg(t);
    need_field(cond, XT_BNONE, XT_BBS, "branch condition");
    need_field(off, -128, 127, "branch displacement");
    return xt_enc_rri8(OP0_B, rfield[cond], s, t, (unsigned)off & 0xff);
}

unsigned long xt_enc_bbi(int set, int s, int bit, long off)
{
    need_reg(s);
    need_field(bit, 0, 31, "bbci/bbsi bit");
    need_field(off, -128, 127, "branch displacement");
    return xt_enc_rri8(OP0_B, (set ? 14 : 6) | (bit >> 4), s, bit & 15,
                       (unsigned)off & 0xff);
}

unsigned long xt_enc_bz(int zcond, int s, long off)
{
    need_reg(s);
    need_field(zcond, XT_BEQZ, XT_BGEZ, "zero-branch condition");
    need_field(off, -2048, 2047, "branch displacement");
    return xt_enc_bri12(OP0_SI, 1, zcond, s, (unsigned)off & 0xfff);
}

/* The sixteen constants the immediate branches can name. */
static const long long B4CONST[16] = {
    -1, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256
};
static const long long B4CONSTU[16] = {
    32768, 65536, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 32, 64, 128, 256
};

static int b4_index(int icond, long long k)
{
    const long long *tab = icond >= XT_BLTUI ? B4CONSTU : B4CONST;
    for (int n = 0; n < 16; n++)
        if (tab[n] == k)
            return n;
    return -1;
}

int xt_bi_ok(int icond, long long k)
{
    return icond >= XT_BEQI && icond <= XT_BGEUI && b4_index(icond, k) >= 0;
}

unsigned long xt_enc_bi(int icond, int s, long long k, long off)
{
    int idx;
    need_reg(s);
    need_field(icond, XT_BEQI, XT_BGEUI, "immediate-branch condition");
    idx = b4_index(icond, k);
    if (idx < 0)
        internal_error("xtensa: %lld is not a constant an immediate branch "
                       "can test", k);
    need_field(off, -128, 127, "branch displacement");
    if (icond >= XT_BLTUI)              /* BI1: m = 2 bltui, 3 bgeui */
        return xt_enc_bri8(OP0_SI, 3, icond - XT_BLTUI + 2, s, idx,
                           (unsigned)off & 0xff);
    return xt_enc_bri8(OP0_SI, 2, icond, s, idx, (unsigned)off & 0xff);
}

unsigned long xt_enc_j(long off)
{
    need_field(off, -131072, 131071, "j displacement");
    return xt_enc_call(OP0_SI, 0, (unsigned long)off & 0x3ffff);
}

/* Which form the word at `at` is, to patch its displacement. */
int xt_patch_branch(struct code *c, int at, int target)
{
    unsigned long w = xt_get(c, at);
    long off = (long)target - (long)(at + 4);
    int op0 = (int)(w & 15), n = (int)(w >> 4) & 3;
    if (op0 == OP0_B || (op0 == OP0_SI && (n == 2 || n == 3))) {
        /* RRI8 or BRI8: imm8 in bits 23..16 (BI1's m=0/1 are entry and
         * the loops, which are never patched here) */
        if (op0 == OP0_SI && n == 3 && ((w >> 6) & 3) < 2)
            internal_error("xtensa: patching an entry or loop at %d", at);
        if (!xt_b_reaches(off))
            return 0;
        xt_put(c, at, (w & 0x00ffffUL) | (((unsigned long)off & 0xff) << 16));
        return 1;
    }
    if (op0 == OP0_SI && n == 1) {                       /* BRI12 */
        if (!xt_bz_reaches(off))
            return 0;
        xt_put(c, at, (w & 0x000fffUL) | (((unsigned long)off & 0xfff) << 12));
        return 1;
    }
    if (op0 == OP0_SI && n == 0) {                       /* j */
        if (!xt_j_reaches(off))
            return 0;
        xt_put(c, at, (w & 0x3fUL) | (((unsigned long)off & 0x3ffff) << 6));
        return 1;
    }
    internal_error("xtensa: the word at %d (0x%06lx) is not a branch", at, w);
    return 0;
}

void xt_call(struct code *c, int n)
{
    need_field(n, 0, 3, "call window increment");
    xt_w(c, xt_enc_call(OP0_CALLN, n, 0));
}

int xt_call_reaches(long at, long target)
{
    long d = target - ((at & ~3L) + 4);
    return (target & 3) == 0 && d >= -524288 && d <= 524284;
}

unsigned long xt_enc_call_to(int n, long at, long target)
{
    long d;
    need_field(n, 0, 3, "call window increment");
    if (target & 3)
        internal_error("xtensa: a call target at %ld is not 4-aligned",
                       target);
    d = target - ((at & ~3L) + 4);
    need_field(d, -524288, 524284, "call displacement");
    return xt_enc_call(OP0_CALLN, n, (unsigned long)(d >> 2) & 0x3ffff);
}

void xt_callx(struct code *c, int n, int s)
{
    need_reg(s);
    need_field(n, 0, 3, "callx window increment");
    st0(c, 0, s, 12 | n);                   /* m = 3 */
}

void xt_jx(struct code *c, int s)
{
    need_reg(s);
    st0(c, 0, s, 10);                       /* m = 2, n = 2 */
}

void xt_ret(struct code *c) { st0(c, 0, 0, 8); }
void xt_retw(struct code *c) { st0(c, 0, 0, 9); }

void xt_entry(struct code *c, int s, long frame)
{
    need_reg(s);
    need_field(frame, 0, 32760, "entry frame size");
    if (frame & 7)
        internal_error("xtensa: entry frame %ld is not a multiple of 8",
                       frame);
    xt_w(c, xt_enc_bri12(OP0_SI, 3, 0, s, (unsigned)(frame >> 3)));
}

void xt_movsp(struct code *c, int t, int s)
{
    need_reg(t); need_reg(s);
    st0(c, 1, s, t);
}

void xt_rotw(struct code *c, int n)
{
    need_field(n, -8, 7, "rotw amount");
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST0, 4, 8, 0, n & 15));
}

int xt_loop_reaches(long off) { return off >= 0 && off <= 255; }

/* BRI8 with n = 3, m = 1 (BI1's LOOPGRP), r = 8, 9 or 10 */
unsigned long xt_enc_loop(int kind, int s, long off)
{
    need_reg(s);
    need_field(kind, XT_LOOP, XT_LOOPGTZ, "loop kind");
    need_field(off, 0, 255, "loop end displacement");
    return xt_enc_bri8(OP0_SI, 3, 1, s, 8 + kind, (unsigned)off);
}

/* ---- the system ---------------------------------------------------------------- */

void xt_nop(struct code *c)     { st0(c, 2, 0, 15); }
void xt_ill(struct code *c)     { st0(c, 0, 0, 0); }
void xt_isync(struct code *c)   { st0(c, 2, 0, 0); }
void xt_rsync(struct code *c)   { st0(c, 2, 0, 1); }
void xt_esync(struct code *c)   { st0(c, 2, 0, 2); }
void xt_dsync(struct code *c)   { st0(c, 2, 0, 3); }
void xt_memw(struct code *c)    { st0(c, 2, 0, 12); }
void xt_extw(struct code *c)    { st0(c, 2, 0, 13); }
void xt_rfe(struct code *c)     { st0(c, 3, 0, 0); }
void xt_rfde(struct code *c)    { st0(c, 3, 2, 0); }
void xt_rfwo(struct code *c)    { st0(c, 3, 4, 0); }
void xt_rfwu(struct code *c)    { st0(c, 3, 5, 0); }
void xt_syscall(struct code *c) { st0(c, 5, 0, 0); }
void xt_simcall(struct code *c) { st0(c, 5, 1, 0); }

void xt_break(struct code *c, int s, int t)
{
    need_field(s, 0, 15, "break code s");
    need_field(t, 0, 15, "break code t");
    st0(c, 4, s, t);
}

void xt_rfi(struct code *c, int level)
{
    need_field(level, 1, 15, "rfi level");
    st0(c, 3, level, 1);
}

void xt_rsil(struct code *c, int t, int level)
{
    need_reg(t);
    need_field(level, 0, 15, "rsil level");
    st0(c, 6, level, t);
}

void xt_waiti(struct code *c, int level)
{
    need_field(level, 0, 15, "waiti level");
    st0(c, 7, level, 0);
}

/* RSR format: the special register number in r:s, bits 15..8. */
static void sr_op(struct code *c, int op1, int op2, int t, int sr)
{
    need_reg(t);
    need_field(sr, 0, 255, "special register");
    xt_w(c, xt_enc_rrr(OP0_QRST, op1, op2, sr >> 4, sr & 15, t));
}

void xt_rsr(struct code *c, int t, int sr) { sr_op(c, OP1_RST3, 0, t, sr); }
void xt_wsr(struct code *c, int t, int sr) { sr_op(c, OP1_RST3, 1, t, sr); }
void xt_xsr(struct code *c, int t, int sr) { sr_op(c, OP1_RST1, 6, t, sr); }

void xt_rur(struct code *c, int r, int ur)
{
    need_reg(r);
    need_field(ur, 0, 255, "user register");
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST3, 14, r, ur >> 4, ur & 15));
}

void xt_wur(struct code *c, int t, int ur)
{
    need_reg(t);
    need_field(ur, 0, 255, "user register");
    xt_w(c, xt_enc_rrr(OP0_QRST, OP1_RST3, 15, ur >> 4, ur & 15, t));
}

int xt_insn_len(int b0)
{
    int op0 = b0 & 15;
    return op0 >= 8 && op0 <= 13 ? 2 : 3;
}
