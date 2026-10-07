/* MIPS32r2 instruction encoding. See emit.h for the shape. */
#include "emit.h"

#include "../../driver/util.h"

const int mips_argreg[MIPS_NARGREG] = { MIPS_A0, MIPS_A1, MIPS_A2, MIPS_A3 };

static const char *const reg_names[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

const char *mips_reg_name(int r)
{
    return r >= 0 && r < 32 ? reg_names[r] : "?";
}

/* The major opcodes and function codes, named as the MIPS32 manual
 * (volume II, table A.2 onwards) names them. */
enum {
    OP_SPECIAL = 0x00, OP_REGIMM = 0x01, OP_J = 0x02, OP_JAL = 0x03,
    OP_BEQ = 0x04, OP_BNE = 0x05, OP_BLEZ = 0x06, OP_BGTZ = 0x07,
    OP_ADDIU = 0x09, OP_SLTI = 0x0a, OP_SLTIU = 0x0b, OP_ANDI = 0x0c,
    OP_ORI = 0x0d, OP_XORI = 0x0e, OP_LUI = 0x0f, OP_COP0 = 0x10,
    OP_SPECIAL2 = 0x1c, OP_SPECIAL3 = 0x1f,
    OP_LB = 0x20, OP_LH = 0x21, OP_LW = 0x23, OP_LBU = 0x24, OP_LHU = 0x25,
    OP_SB = 0x28, OP_SH = 0x29, OP_SW = 0x2b, OP_LL = 0x30, OP_SC = 0x38,
    OP_LWL = 0x22, OP_LWR = 0x26, OP_SWL = 0x2a, OP_SWR = 0x2e
};
enum {                                              /* SPECIAL's funct */
    F_SLL = 0x00, F_SRL = 0x02, F_SRA = 0x03, F_SLLV = 0x04, F_SRLV = 0x06,
    F_SRAV = 0x07, F_JR = 0x08, F_JALR = 0x09, F_MOVZ = 0x0a, F_MOVN = 0x0b,
    F_SYSCALL = 0x0c, F_BREAK = 0x0d, F_SYNC = 0x0f, F_MFHI = 0x10,
    F_MTHI = 0x11, F_MFLO = 0x12, F_MTLO = 0x13, F_MULT = 0x18,
    F_MULTU = 0x19, F_DIV = 0x1a, F_DIVU = 0x1b, F_ADDU = 0x21,
    F_SUBU = 0x23, F_AND = 0x24, F_OR = 0x25, F_XOR = 0x26, F_NOR = 0x27,
    F_SLT = 0x2a, F_SLTU = 0x2b, F_TEQ = 0x34
};
enum { F2_MUL = 0x02, F2_CLZ = 0x20, F2_CLO = 0x21 };   /* SPECIAL2 */
enum { F3_EXT = 0x00, F3_INS = 0x04, F3_BSHFL = 0x20 };  /* SPECIAL3 */
enum { BSHFL_WSBH = 0x02, BSHFL_SEB = 0x10, BSHFL_SEH = 0x18 };
enum { RI_BLTZ = 0x00, RI_BGEZ = 0x01, RI_BGEZAL = 0x11 }; /* REGIMM's rt */

static void need_reg(int r)
{
    if (r < 0 || r > 31)
        internal_error("mips: register %d is not $0-$31", r);
}

static void need_field(long long v, long long lo, long long hi,
                       const char *what)
{
    if (v < lo || v > hi)
        internal_error("mips: %s %lld is outside %lld..%lld", what, v, lo, hi);
}

int mips_fits16(long long v, int sign)
{
    return sign ? v >= -32768 && v <= 32767 : v >= 0 && v <= 65535;
}

unsigned long mips_enc_r(int op, int rs, int rt, int rd, int sa, int funct)
{
    need_reg(rs); need_reg(rt); need_reg(rd);
    need_field(sa, 0, 31, "shift/sa field");
    return ((unsigned long)op << 26) | ((unsigned long)rs << 21) |
           ((unsigned long)rt << 16) | ((unsigned long)rd << 11) |
           ((unsigned long)sa << 6) | (unsigned long)funct;
}

unsigned long mips_enc_i(int op, int rs, int rt, unsigned imm)
{
    need_reg(rs); need_reg(rt);
    need_field(imm, 0, 0xffff, "16-bit field");
    return ((unsigned long)op << 26) | ((unsigned long)rs << 21) |
           ((unsigned long)rt << 16) | (unsigned long)imm;
}

unsigned long mips_enc_j(int op, unsigned long target26)
{
    need_field((long long)target26, 0, 0x3ffffff, "jump target");
    return ((unsigned long)op << 26) | target26;
}

/* The byte order the words go out in. Here rather than read from
 * target.h, because EmbLD links this file without the target module and
 * decides the order from the objects it reads. */
static int g_mips_be;

void mips_set_big_endian(int on) { g_mips_be = on ? 1 : 0; }
int mips_big_endian(void) { return g_mips_be; }

void mips_put_word(unsigned char *p, unsigned long w)
{
    for (int b = 0; b < 4; b++)
        p[g_mips_be ? 3 - b : b] = (unsigned char)(w >> (8 * b));
}

unsigned long mips_get_word(const unsigned char *p)
{
    unsigned long w = 0;
    for (int b = 0; b < 4; b++)
        w |= (unsigned long)p[g_mips_be ? 3 - b : b] << (8 * b);
    return w;
}

void mips_w(struct code *c, unsigned long w)
{
    unsigned char b[4];
    mips_put_word(b, w & 0xffffffffUL);
    for (int k = 0; k < 4; k++)
        code_byte(c, b[k]);
}

unsigned long mips_rdw(const struct code *c, int at)
{
    return mips_get_word(c->p + at);
}

void mips_wrw(struct code *c, int at, unsigned long w)
{
    mips_put_word(c->p + at, w & 0xffffffffUL);
}

/* A signed or unsigned value as the 16-bit field that extends back to it. */
static unsigned imm16(long long v, int sign, const char *what)
{
    if (!mips_fits16(v, sign))
        internal_error("mips: %s: %lld does not fit a %s 16-bit field",
                       what, v, sign ? "sign-extended" : "zero-extended");
    return (unsigned)(v & 0xffff);
}

/* ---- moves and constants ---------------------------------------------- */

void mips_nop(struct code *c) { mips_w(c, 0); }

void mips_mv(struct code *c, int rd, int rs)
{
    mips_w(c, mips_enc_r(OP_SPECIAL, rs, MIPS_ZERO, rd, 0, F_OR));
}

void mips_lui(struct code *c, int rt, unsigned imm)
{
    mips_w(c, mips_enc_i(OP_LUI, MIPS_ZERO, rt, imm));
}

/* The value as the register will hold it: the low 32 bits, sign-extended
 * so a caller passing 0xffffffff and one passing -1 agree. */
static long long low32(long long v)
{
    return (long long)(int)(unsigned int)(unsigned long long)v;
}

int mips_li_len(long long v)
{
    v = low32(v);
    if (mips_fits16(v, 1) || mips_fits16(v, 0))
        return 4;
    return (v & 0xffff) ? 8 : 4;
}

void mips_li(struct code *c, int rd, long long v)
{
    v = low32(v);
    if (mips_fits16(v, 1)) {
        mips_alu_imm(c, MIPS_ADDIU, rd, MIPS_ZERO, v);
        return;
    }
    if (mips_fits16(v, 0)) {
        mips_alu_imm(c, MIPS_ORI, rd, MIPS_ZERO, v);
        return;
    }
    mips_lui(c, rd, (unsigned)((unsigned long long)v >> 16) & 0xffff);
    if (v & 0xffff)
        mips_alu_imm(c, MIPS_ORI, rd, rd, v & 0xffff);
}

/* ---- arithmetic and logic --------------------------------------------- */

void mips_alu(struct code *c, int op, int rd, int a, int b)
{
    unsigned long w;
    switch (op) {
    case MIPS_ADDU:  w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_ADDU); break;
    case MIPS_SUBU:  w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_SUBU); break;
    case MIPS_AND:   w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_AND); break;
    case MIPS_OR:    w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_OR); break;
    case MIPS_XOR:   w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_XOR); break;
    case MIPS_NOR:   w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_NOR); break;
    case MIPS_SLT:   w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_SLT); break;
    case MIPS_SLTU:  w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_SLTU); break;
    /* The variable shifts put the AMOUNT in rs and the value in rt. */
    case MIPS_SLLV:  w = mips_enc_r(OP_SPECIAL, b, a, rd, 0, F_SLLV); break;
    case MIPS_SRLV:  w = mips_enc_r(OP_SPECIAL, b, a, rd, 0, F_SRLV); break;
    case MIPS_SRAV:  w = mips_enc_r(OP_SPECIAL, b, a, rd, 0, F_SRAV); break;
    /* rotrv is srlv with the sa field's low bit set (Release 2). */
    case MIPS_ROTRV: w = mips_enc_r(OP_SPECIAL, b, a, rd, 1, F_SRLV); break;
    case MIPS_MOVN:  w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_MOVN); break;
    case MIPS_MOVZ:  w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_MOVZ); break;
    case MIPS_MUL:   w = mips_enc_r(OP_SPECIAL2, a, b, rd, 0, F2_MUL); break;
    default:
        internal_error("mips: ALU operation %d", op);
    }
    mips_w(c, w);
}

int mips_alu_imm_ok(int op, long long imm)
{
    switch (op) {
    case MIPS_ADDIU: case MIPS_SLTI: case MIPS_SLTIU:
        return mips_fits16(imm, 1);
    case MIPS_ANDI: case MIPS_ORI: case MIPS_XORI:
        return mips_fits16(imm, 0);
    default:
        return 0;
    }
}

void mips_alu_imm(struct code *c, int op, int rt, int rs, long long imm)
{
    static const int opc[] = { OP_ADDIU, OP_SLTI, OP_SLTIU, OP_ANDI,
                               OP_ORI, OP_XORI };
    static const char *const nm[] = { "addiu", "slti", "sltiu", "andi",
                                      "ori", "xori" };
    if (op < MIPS_ADDIU || op > MIPS_XORI)
        internal_error("mips: immediate operation %d", op);
    mips_w(c, mips_enc_i(opc[op], rs, rt,
                         imm16(imm, op <= MIPS_SLTIU, nm[op])));
}

void mips_shift_imm(struct code *c, int op, int rd, int rt, int sa)
{
    need_field(sa, 0, 31, "shift amount");
    switch (op) {
    case MIPS_SLL:  mips_w(c, mips_enc_r(OP_SPECIAL, 0, rt, rd, sa, F_SLL)); break;
    case MIPS_SRL:  mips_w(c, mips_enc_r(OP_SPECIAL, 0, rt, rd, sa, F_SRL)); break;
    case MIPS_SRA:  mips_w(c, mips_enc_r(OP_SPECIAL, 0, rt, rd, sa, F_SRA)); break;
    /* rotr is srl with the rs field's low bit set (Release 2). */
    case MIPS_ROTR: mips_w(c, mips_enc_r(OP_SPECIAL, 1, rt, rd, sa, F_SRL)); break;
    default:
        internal_error("mips: shift operation %d", op);
    }
}

void mips_muldiv(struct code *c, int op, int rs, int rt)
{
    static const int fn[] = { F_MULT, F_MULTU, F_DIV, F_DIVU };
    if (op < MIPS_MULT || op > MIPS_DIVU)
        internal_error("mips: multiply/divide operation %d", op);
    mips_w(c, mips_enc_r(OP_SPECIAL, rs, rt, MIPS_ZERO, 0, fn[op]));
}

void mips_mfhi(struct code *c, int rd)
{
    mips_w(c, mips_enc_r(OP_SPECIAL, 0, 0, rd, 0, F_MFHI));
}
void mips_mflo(struct code *c, int rd)
{
    mips_w(c, mips_enc_r(OP_SPECIAL, 0, 0, rd, 0, F_MFLO));
}
void mips_mthi(struct code *c, int rs)
{
    mips_w(c, mips_enc_r(OP_SPECIAL, rs, 0, 0, 0, F_MTHI));
}
void mips_mtlo(struct code *c, int rs)
{
    mips_w(c, mips_enc_r(OP_SPECIAL, rs, 0, 0, 0, F_MTLO));
}

/* clz and clo name their destination twice, in rt and rd: the manual
 * requires the two fields to agree. */
void mips_clz(struct code *c, int rd, int rs)
{
    mips_w(c, mips_enc_r(OP_SPECIAL2, rs, rd, rd, 0, F2_CLZ));
}
void mips_clo(struct code *c, int rd, int rs)
{
    mips_w(c, mips_enc_r(OP_SPECIAL2, rs, rd, rd, 0, F2_CLO));
}

void mips_seb(struct code *c, int rd, int rt)
{
    mips_w(c, mips_enc_r(OP_SPECIAL3, 0, rt, rd, BSHFL_SEB, F3_BSHFL));
}
void mips_seh(struct code *c, int rd, int rt)
{
    mips_w(c, mips_enc_r(OP_SPECIAL3, 0, rt, rd, BSHFL_SEH, F3_BSHFL));
}
void mips_wsbh(struct code *c, int rd, int rt)
{
    mips_w(c, mips_enc_r(OP_SPECIAL3, 0, rt, rd, BSHFL_WSBH, F3_BSHFL));
}

/* ext keeps size-1 in the rd field and ins the field's last bit,
 * pos+size-1: two different encodings of one (pos, size) pair, which is
 * exactly the sort of thing to derive once. */
void mips_ext(struct code *c, int rt, int rs, int pos, int size)
{
    need_field(pos, 0, 31, "ext position");
    need_field(size, 1, 32 - pos, "ext size");
    mips_w(c, mips_enc_r(OP_SPECIAL3, rs, rt, size - 1, pos, F3_EXT));
}
void mips_ins(struct code *c, int rt, int rs, int pos, int size)
{
    need_field(pos, 0, 31, "ins position");
    need_field(size, 1, 32 - pos, "ins size");
    mips_w(c, mips_enc_r(OP_SPECIAL3, rs, rt, pos + size - 1, pos, F3_INS));
}

/* ---- memory ----------------------------------------------------------- */

void mips_load(struct code *c, int rt, int base, int off, int size, int sign)
{
    int op;
    switch (size) {
    case 1: op = sign ? OP_LB : OP_LBU; break;
    case 2: op = sign ? OP_LH : OP_LHU; break;
    case 4: op = OP_LW; break;
    default:
        internal_error("mips: a %d-byte load", size);
    }
    mips_w(c, mips_enc_i(op, base, rt, imm16(off, 1, "load offset")));
}

void mips_store(struct code *c, int rt, int base, int off, int size)
{
    int op;
    switch (size) {
    case 1: op = OP_SB; break;
    case 2: op = OP_SH; break;
    case 4: op = OP_SW; break;
    default:
        internal_error("mips: a %d-byte store", size);
    }
    mips_w(c, mips_enc_i(op, base, rt, imm16(off, 1, "store offset")));
}

void mips_lwl(struct code *c, int rt, int base, int off)
{
    mips_w(c, mips_enc_i(OP_LWL, base, rt, imm16(off, 1, "lwl offset")));
}
void mips_lwr(struct code *c, int rt, int base, int off)
{
    mips_w(c, mips_enc_i(OP_LWR, base, rt, imm16(off, 1, "lwr offset")));
}
void mips_swl(struct code *c, int rt, int base, int off)
{
    mips_w(c, mips_enc_i(OP_SWL, base, rt, imm16(off, 1, "swl offset")));
}
void mips_swr(struct code *c, int rt, int base, int off)
{
    mips_w(c, mips_enc_i(OP_SWR, base, rt, imm16(off, 1, "swr offset")));
}

void mips_ll(struct code *c, int rt, int base, int off)
{
    mips_w(c, mips_enc_i(OP_LL, base, rt, imm16(off, 1, "ll offset")));
}
void mips_sc(struct code *c, int rt, int base, int off)
{
    mips_w(c, mips_enc_i(OP_SC, base, rt, imm16(off, 1, "sc offset")));
}
void mips_sync(struct code *c, int stype)
{
    need_field(stype, 0, 31, "sync type");
    mips_w(c, mips_enc_r(OP_SPECIAL, 0, 0, 0, stype, F_SYNC));
}

/* ---- control flow ----------------------------------------------------- */

unsigned long mips_enc_branch(int cond, int rs, int rt, long off)
{
    unsigned field;
    if (off & 3)
        internal_error("mips: branch displacement %ld is not a multiple of 4",
                       off);
    need_field(off, -131072, 131068, "branch displacement");
    field = (unsigned)((off >> 2) & 0xffff);
    switch (cond) {
    case MIPS_BEQ:  return mips_enc_i(OP_BEQ, rs, rt, field);
    case MIPS_BNE:  return mips_enc_i(OP_BNE, rs, rt, field);
    case MIPS_BLEZ: return mips_enc_i(OP_BLEZ, rs, 0, field);
    case MIPS_BGTZ: return mips_enc_i(OP_BGTZ, rs, 0, field);
    case MIPS_BLTZ: return mips_enc_i(OP_REGIMM, rs, RI_BLTZ, field);
    case MIPS_BGEZ: return mips_enc_i(OP_REGIMM, rs, RI_BGEZ, field);
    case MIPS_BAL:  return mips_enc_i(OP_REGIMM, 0, RI_BGEZAL, field);
    default:
        internal_error("mips: branch condition %d", cond);
    }
}

int mips_b_placeholder(struct code *c, int cond, int rs, int rt)
{
    int at = c->len;
    mips_w(c, mips_enc_branch(cond, rs, rt, 0));
    return at;
}

int mips_b_reaches(long at, long target)
{
    long off = target - (at + 4);
    return !(off & 3) && off >= -131072 && off <= 131068;
}

int mips_patch_b(struct code *c, int at, int target)
{
    unsigned long w;
    long off = (long)target - (at + 4);
    if (!mips_b_reaches(at, target))
        return 0;
    w = mips_rdw(c, at);
    w = (w & 0xffff0000UL) | (unsigned long)((off >> 2) & 0xffff);
    mips_wrw(c, at, w);
    return 1;
}

void mips_jal(struct code *c) { mips_w(c, mips_enc_j(OP_JAL, 0)); }
void mips_j(struct code *c)   { mips_w(c, mips_enc_j(OP_J, 0)); }

void mips_jr(struct code *c, int rs)
{
    mips_w(c, mips_enc_r(OP_SPECIAL, rs, 0, 0, 0, F_JR));
}
void mips_jalr(struct code *c, int rd, int rs)
{
    mips_w(c, mips_enc_r(OP_SPECIAL, rs, 0, rd, 0, F_JALR));
}

/* ---- traps and the system --------------------------------------------- */

/* break's code is the upper ten bits of its 20-bit field, which is where
 * gas and llvm-mc put a one-operand `break n`. */
void mips_break(struct code *c, int code)
{
    need_field(code, 0, 1023, "break code");
    mips_w(c, ((unsigned long)code << 16) | F_BREAK);
}
void mips_syscall(struct code *c) { mips_w(c, F_SYSCALL); }
void mips_teq(struct code *c, int rs, int rt, int code)
{
    need_reg(rs); need_reg(rt);
    need_field(code, 0, 1023, "trap code");
    mips_w(c, ((unsigned long)rs << 21) | ((unsigned long)rt << 16) |
              ((unsigned long)code << 6) | F_TEQ);
}
/* COP0 with the CO bit: eret and wait. */
void mips_eret(struct code *c) { mips_w(c, 0x42000018UL); }
void mips_wait(struct code *c) { mips_w(c, 0x42000020UL); }
void mips_ehb(struct code *c)
{
    mips_w(c, mips_enc_r(OP_SPECIAL, 0, 0, 0, 3, F_SLL));     /* sll $0,$0,3 */
}
/* MFMC0 (rs = 0b01011), rd = 12 (Status), the sc bit selecting ei. */
void mips_di(struct code *c, int rt)
{
    mips_w(c, mips_enc_r(OP_COP0, 0x0b, rt, 12, 0, 0x00));
}
void mips_ei(struct code *c, int rt)
{
    mips_w(c, mips_enc_r(OP_COP0, 0x0b, rt, 12, 0, 0x20));
}
void mips_mfc0(struct code *c, int rt, int rd, int sel)
{
    need_field(sel, 0, 7, "coprocessor 0 select");
    mips_w(c, mips_enc_r(OP_COP0, 0x00, rt, rd, 0, sel));
}
void mips_mtc0(struct code *c, int rt, int rd, int sel)
{
    need_field(sel, 0, 7, "coprocessor 0 select");
    mips_w(c, mips_enc_r(OP_COP0, 0x04, rt, rd, 0, sel));
}
