/* MIPS32r2 and MIPS64r2 instruction encoding. See emit.h for the shape. */
#include "emit.h"

#include "../../driver/util.h"

/* MIPS64: the doubleword instructions. Off by default, so a MIPS32
 * object can never carry one -- each is refused with the switch off. */
static int g_mips_64;

const int mips_argreg[MIPS_NARGREG] = { MIPS_A0, MIPS_A1, MIPS_A2, MIPS_A3 };

static const char *const reg_names[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

/* n64 renames $8-$15: the four argument registers past a3 and then the
 * temporaries t0-t3 (o32's t4-t7). */
static const char *const reg_names64[8] = {
    "a4", "a5", "a6", "a7", "t0", "t1", "t2", "t3"
};

const char *mips_reg_name(int r)
{
    if (g_mips_64 && r >= 8 && r <= 15)
        return reg_names64[r - 8];
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
    OP_LWL = 0x22, OP_LWR = 0x26, OP_SWL = 0x2a, OP_SWR = 0x2e,
    /* MIPS64 */
    OP_DADDIU = 0x19, OP_LDL = 0x1a, OP_LDR = 0x1b, OP_LWU = 0x27,
    OP_SDL = 0x2c, OP_SDR = 0x2d, OP_LLD = 0x34, OP_LD = 0x37,
    OP_SCD = 0x3c, OP_SD = 0x3f
};
enum {                                              /* SPECIAL's funct */
    F_SLL = 0x00, F_SRL = 0x02, F_SRA = 0x03, F_SLLV = 0x04, F_SRLV = 0x06,
    F_SRAV = 0x07, F_JR = 0x08, F_JALR = 0x09, F_MOVZ = 0x0a, F_MOVN = 0x0b,
    F_SYSCALL = 0x0c, F_BREAK = 0x0d, F_SYNC = 0x0f, F_MFHI = 0x10,
    F_MTHI = 0x11, F_MFLO = 0x12, F_MTLO = 0x13, F_MULT = 0x18,
    F_MULTU = 0x19, F_DIV = 0x1a, F_DIVU = 0x1b, F_ADDU = 0x21,
    F_SUBU = 0x23, F_AND = 0x24, F_OR = 0x25, F_XOR = 0x26, F_NOR = 0x27,
    F_SLT = 0x2a, F_SLTU = 0x2b, F_TEQ = 0x34,
    /* MIPS64 */
    F_DSLLV = 0x14, F_DSRLV = 0x16, F_DSRAV = 0x17, F_DMULT = 0x1c,
    F_DMULTU = 0x1d, F_DDIV = 0x1e, F_DDIVU = 0x1f, F_DADDU = 0x2d,
    F_DSUBU = 0x2f, F_DSLL = 0x38, F_DSRL = 0x3a, F_DSRA = 0x3b,
    F_DSLL32 = 0x3c, F_DSRL32 = 0x3e, F_DSRA32 = 0x3f
};
enum { F2_MUL = 0x02, F2_CLZ = 0x20, F2_CLO = 0x21,     /* SPECIAL2 */
       F2_DCLZ = 0x24, F2_DCLO = 0x25 };
enum { F3_EXT = 0x00, F3_DEXTM = 0x01, F3_DEXTU = 0x02,  /* SPECIAL3 */
       F3_DEXT = 0x03, F3_INS = 0x04, F3_DINSM = 0x05, F3_DINSU = 0x06,
       F3_DINS = 0x07, F3_BSHFL = 0x20, F3_DBSHFL = 0x24 };
enum { BSHFL_WSBH = 0x02, BSHFL_SEB = 0x10, BSHFL_SEH = 0x18 };
enum { DBSHFL_DSBH = 0x02, DBSHFL_DSHD = 0x05 };
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

void mips_set_64(int on) { g_mips_64 = on ? 1 : 0; }
int mips_is_64(void) { return g_mips_64; }

static void need_64(const char *what)
{
    if (!g_mips_64)
        internal_error("mips: %s is a MIPS64 instruction", what);
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

/* MIPS64 constants. A value that is the sign extension of its low 32 bits
 * is mips_li's: lui and addiu sign-extend into the upper half here, and
 * ori from $0 zero-extends a 16-bit field, so the 32-bit sequences mean
 * the same 64-bit value. Anything wider is the shortest of
 *
 *   - v >> s (s the trailing zeros), built recursively, then dsll s;
 *   - v >> 16 (arithmetic) built recursively, dsll 16, ori the low half;
 *   - the zero extension of a 32-bit value: that value, then dext 0, 32.
 *
 * li64_plan writes the sequence when `c` is not NULL and returns its
 * length in instructions either way. */
static int li64_plan(struct code *c, int rd, long long v);

static int li64_len_of(long long v)
{
    return li64_plan((struct code *)0, 0, v);
}

static int li64_plan(struct code *c, int rd, long long v)
{
    unsigned long long u = (unsigned long long)v;
    int best, how = 0, n, s = 0;
    if (v == (long long)(int)v) {
        n = mips_li_len(v) / 4;
        if (c)
            mips_li(c, rd, v);
        return n;
    }
    /* by the trailing zeros */
    while (!((u >> s) & 1))
        s++;
    best = 1 << 30;
    if (s) {
        n = li64_len_of(v >> s) + 1;
        if (n < best) { best = n; how = 1; }
    }
    /* the zero extension of a 32-bit value */
    if (!(u >> 32)) {
        n = mips_li_len((long long)(int)(unsigned)u) / 4 + 1;
        if (n < best) { best = n; how = 2; }
    }
    /* sixteen bits at a time from the top */
    n = li64_len_of(v >> 16) + 1 + ((u & 0xffff) != 0);
    if (n < best) { best = n; how = 3; }
    if (!c)
        return best;
    switch (how) {
    case 1:
        li64_plan(c, rd, v >> s);
        mips_shift_imm(c, MIPS_DSLL, rd, rd, s);
        break;
    case 2:
        mips_li(c, rd, (long long)(int)(unsigned)u);
        mips_dext(c, rd, rd, 0, 32);
        break;
    default:
        li64_plan(c, rd, v >> 16);
        mips_shift_imm(c, MIPS_DSLL, rd, rd, 16);
        if (u & 0xffff)
            mips_alu_imm(c, MIPS_ORI, rd, rd, (long long)(u & 0xffff));
        break;
    }
    return best;
}

int mips_li64_len(long long v)
{
    return 4 * li64_len_of(v);
}

void mips_li64(struct code *c, int rd, long long v)
{
    if (v != (long long)(int)v)
        need_64("a 64-bit constant");
    li64_plan(c, rd, v);
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
    case MIPS_DADDU: need_64("daddu");
                     w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_DADDU); break;
    case MIPS_DSUBU: need_64("dsubu");
                     w = mips_enc_r(OP_SPECIAL, a, b, rd, 0, F_DSUBU); break;
    case MIPS_DSLLV: need_64("dsllv");
                     w = mips_enc_r(OP_SPECIAL, b, a, rd, 0, F_DSLLV); break;
    case MIPS_DSRLV: need_64("dsrlv");
                     w = mips_enc_r(OP_SPECIAL, b, a, rd, 0, F_DSRLV); break;
    case MIPS_DSRAV: need_64("dsrav");
                     w = mips_enc_r(OP_SPECIAL, b, a, rd, 0, F_DSRAV); break;
    /* drotrv is dsrlv with the sa field's low bit set */
    case MIPS_DROTRV: need_64("drotrv");
                     w = mips_enc_r(OP_SPECIAL, b, a, rd, 1, F_DSRLV); break;
    default:
        internal_error("mips: ALU operation %d", op);
    }
    mips_w(c, w);
}

int mips_alu_imm_signed(int op)
{
    return op == MIPS_ADDIU || op == MIPS_SLTI || op == MIPS_SLTIU ||
           op == MIPS_DADDIU;
}

int mips_alu_imm_ok(int op, long long imm)
{
    switch (op) {
    case MIPS_ADDIU: case MIPS_SLTI: case MIPS_SLTIU: case MIPS_DADDIU:
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
                               OP_ORI, OP_XORI, OP_DADDIU };
    static const char *const nm[] = { "addiu", "slti", "sltiu", "andi",
                                      "ori", "xori", "daddiu" };
    if (op < MIPS_ADDIU || op > MIPS_DADDIU)
        internal_error("mips: immediate operation %d", op);
    if (op == MIPS_DADDIU)
        need_64("daddiu");
    mips_w(c, mips_enc_i(opc[op], rs, rt,
                         imm16(imm, mips_alu_imm_signed(op), nm[op])));
}

void mips_shift_imm(struct code *c, int op, int rd, int rt, int sa)
{
    if (op >= MIPS_DSLL && op <= MIPS_DROTR) {
        /* sa 32..63 is the *32 form with sa - 32 in the field */
        static const int f[] = { F_DSLL, F_DSRL, F_DSRA, F_DSRL };
        static const int f32[] = { F_DSLL32, F_DSRL32, F_DSRA32, F_DSRL32 };
        int k = op - MIPS_DSLL;
        need_64("a doubleword shift");
        need_field(sa, 0, 63, "doubleword shift amount");
        mips_w(c, mips_enc_r(OP_SPECIAL, op == MIPS_DROTR, rt, rd, sa & 31,
                             sa >= 32 ? f32[k] : f[k]));
        return;
    }
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
    static const int fn[] = { F_MULT, F_MULTU, F_DIV, F_DIVU,
                              F_DMULT, F_DMULTU, F_DDIV, F_DDIVU };
    if (op < MIPS_MULT || op > MIPS_DDIVU)
        internal_error("mips: multiply/divide operation %d", op);
    if (op >= MIPS_DMULT)
        need_64("a doubleword multiply or divide");
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

/* The doubleword field instructions: three encodings each, chosen by
 * where the field lies. dext keeps size-1 in rd and pos in sa (both
 * below 32); dextm a size over 32 (size-33 in rd); dextu a pos of 32 or
 * more (pos-32 in sa). dins keeps the last bit, pos+size-1: dinsm when
 * that is 32 or more and pos is not, dinsu when pos is. */
void mips_dext(struct code *c, int rt, int rs, int pos, int size)
{
    need_64("dext");
    need_field(pos, 0, 63, "dext position");
    need_field(size, 1, 64 - pos, "dext size");
    if (pos >= 32)
        mips_w(c, mips_enc_r(OP_SPECIAL3, rs, rt, size - 1, pos - 32,
                             F3_DEXTU));
    else if (size > 32)
        mips_w(c, mips_enc_r(OP_SPECIAL3, rs, rt, size - 33, pos, F3_DEXTM));
    else
        mips_w(c, mips_enc_r(OP_SPECIAL3, rs, rt, size - 1, pos, F3_DEXT));
}
void mips_dins(struct code *c, int rt, int rs, int pos, int size)
{
    int msb = pos + size - 1;
    need_64("dins");
    need_field(pos, 0, 63, "dins position");
    need_field(size, 1, 64 - pos, "dins size");
    if (pos >= 32)
        mips_w(c, mips_enc_r(OP_SPECIAL3, rs, rt, msb - 32, pos - 32,
                             F3_DINSU));
    else if (msb >= 32)
        mips_w(c, mips_enc_r(OP_SPECIAL3, rs, rt, msb - 32, pos, F3_DINSM));
    else
        mips_w(c, mips_enc_r(OP_SPECIAL3, rs, rt, msb, pos, F3_DINS));
}
void mips_dsbh(struct code *c, int rd, int rt)
{
    need_64("dsbh");
    mips_w(c, mips_enc_r(OP_SPECIAL3, 0, rt, rd, DBSHFL_DSBH, F3_DBSHFL));
}
void mips_dshd(struct code *c, int rd, int rt)
{
    need_64("dshd");
    mips_w(c, mips_enc_r(OP_SPECIAL3, 0, rt, rd, DBSHFL_DSHD, F3_DBSHFL));
}
void mips_dclz(struct code *c, int rd, int rs)
{
    need_64("dclz");
    mips_w(c, mips_enc_r(OP_SPECIAL2, rs, rd, rd, 0, F2_DCLZ));
}
void mips_dclo(struct code *c, int rd, int rs)
{
    need_64("dclo");
    mips_w(c, mips_enc_r(OP_SPECIAL2, rs, rd, rd, 0, F2_DCLO));
}

/* ---- memory ----------------------------------------------------------- */

void mips_load(struct code *c, int rt, int base, int off, int size, int sign)
{
    int op;
    switch (size) {
    case 1: op = sign ? OP_LB : OP_LBU; break;
    case 2: op = sign ? OP_LH : OP_LHU; break;
    case 4: op = OP_LW; break;
    case 8: need_64("ld"); op = OP_LD; break;
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
    case 8: need_64("sd"); op = OP_SD; break;
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

void mips_lwu(struct code *c, int rt, int base, int off)
{
    need_64("lwu");
    mips_w(c, mips_enc_i(OP_LWU, base, rt, imm16(off, 1, "lwu offset")));
}
void mips_ldl(struct code *c, int rt, int base, int off)
{
    need_64("ldl");
    mips_w(c, mips_enc_i(OP_LDL, base, rt, imm16(off, 1, "ldl offset")));
}
void mips_ldr(struct code *c, int rt, int base, int off)
{
    need_64("ldr");
    mips_w(c, mips_enc_i(OP_LDR, base, rt, imm16(off, 1, "ldr offset")));
}
void mips_sdl(struct code *c, int rt, int base, int off)
{
    need_64("sdl");
    mips_w(c, mips_enc_i(OP_SDL, base, rt, imm16(off, 1, "sdl offset")));
}
void mips_sdr(struct code *c, int rt, int base, int off)
{
    need_64("sdr");
    mips_w(c, mips_enc_i(OP_SDR, base, rt, imm16(off, 1, "sdr offset")));
}
void mips_lld(struct code *c, int rt, int base, int off)
{
    need_64("lld");
    mips_w(c, mips_enc_i(OP_LLD, base, rt, imm16(off, 1, "lld offset")));
}
void mips_scd(struct code *c, int rt, int base, int off)
{
    need_64("scd");
    mips_w(c, mips_enc_i(OP_SCD, base, rt, imm16(off, 1, "scd offset")));
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
void mips_dmfc0(struct code *c, int rt, int rd, int sel)
{
    need_64("dmfc0");
    need_field(sel, 0, 7, "coprocessor 0 select");
    mips_w(c, mips_enc_r(OP_COP0, 0x01, rt, rd, 0, sel));
}
void mips_dmtc0(struct code *c, int rt, int rd, int sel)
{
    need_64("dmtc0");
    need_field(sel, 0, 7, "coprocessor 0 select");
    mips_w(c, mips_enc_r(OP_COP0, 0x05, rt, rd, 0, sel));
}
