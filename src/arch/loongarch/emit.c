/* LoongArch64 instruction encoding. See emit.h for the shape of it.
 *
 * Every opcode word below was read off `llvm-mc --triple=loongarch64
 * -show-encoding` with every field zero, not transcribed from a manual
 * table; tools/lacheck sweeps every field of every form against llvm-mc
 * again on each run of tests/golden/loongarch-encoding.sh. */
#include "emit.h"

#include "../../driver/util.h"

const int la_argreg[LA_NARGREG] = {
    LA_A0, LA_A1, LA_A2, LA_A3, LA_A4, LA_A5, LA_A6, LA_A7
};

static const char *const la_names[32] = {
    "zero", "ra", "tp", "sp", "a0", "a1", "a2", "a3",
    "a4",   "a5", "a6", "a7", "t0", "t1", "t2", "t3",
    "t4",   "t5", "t6", "t7", "t8", "r21", "fp", "s0",
    "s1",   "s2", "s3", "s4", "s5", "s6", "s7", "s8"
};

const char *la_reg_name(int r)
{
    return r >= 0 && r < 32 ? la_names[r] : "?";
}

int la_fits(long long v, int bits)
{
    long long lo = -(1LL << (bits - 1)), hi = (1LL << (bits - 1)) - 1;
    return v >= lo && v <= hi;
}

int la_ufits(long long v, int bits)
{
    return v >= 0 && v <= (1LL << bits) - 1;
}

static void need_reg(int r)
{
    if (r < 0 || r > 31)
        internal_error("loongarch: register %d is not r0-r31", r);
}

static void need_s(long long v, int bits, const char *what)
{
    if (!la_fits(v, bits))
        internal_error("loongarch: %s: %lld does not fit in %d signed bits",
                       what, v, bits);
}

static void need_u(long long v, int bits, const char *what)
{
    if (!la_ufits(v, bits))
        internal_error("loongarch: %s: %lld does not fit in %d unsigned bits",
                       what, v, bits);
}

/* ---- the formats ---------------------------------------------------- */

unsigned long la_enc_2r(unsigned long op, int rd, int rj)
{
    need_reg(rd); need_reg(rj);
    return op | ((unsigned long)rj << 5) | (unsigned long)rd;
}

unsigned long la_enc_3r(unsigned long op, int rd, int rj, int rk)
{
    need_reg(rd); need_reg(rj); need_reg(rk);
    return op | ((unsigned long)rk << 10) | ((unsigned long)rj << 5) |
           (unsigned long)rd;
}

unsigned long la_enc_2ri12(unsigned long op, int rd, int rj, unsigned imm12)
{
    need_reg(rd); need_reg(rj);
    return op | ((unsigned long)(imm12 & 0xfffu) << 10) |
           ((unsigned long)rj << 5) | (unsigned long)rd;
}

unsigned long la_enc_2ri14(unsigned long op, int rd, int rj, unsigned imm14)
{
    need_reg(rd); need_reg(rj);
    return op | ((unsigned long)(imm14 & 0x3fffu) << 10) |
           ((unsigned long)rj << 5) | (unsigned long)rd;
}

unsigned long la_enc_2ri16(unsigned long op, int rd, int rj, unsigned imm16)
{
    need_reg(rd); need_reg(rj);
    return op | ((unsigned long)(imm16 & 0xffffu) << 10) |
           ((unsigned long)rj << 5) | (unsigned long)rd;
}

unsigned long la_enc_1ri20(unsigned long op, int rd, unsigned imm20)
{
    need_reg(rd);
    return op | ((unsigned long)(imm20 & 0xfffffu) << 5) | (unsigned long)rd;
}

/* The 21-bit offset of beqz/bnez is split: its low 16 bits sit where a
 * 2RI16's field does, and its high 5 where rd would be. */
unsigned long la_enc_1ri21(unsigned long op, int rj, unsigned imm21)
{
    need_reg(rj);
    return op | ((unsigned long)(imm21 & 0xffffu) << 10) |
           ((unsigned long)rj << 5) | (unsigned long)((imm21 >> 16) & 0x1fu);
}

/* ...and b/bl's 26 bits likewise: the low 16 at 25:10, the high 10 at
 * 9:0. */
unsigned long la_enc_i26(unsigned long op, unsigned imm26)
{
    return op | ((unsigned long)(imm26 & 0xffffu) << 10) |
           (unsigned long)((imm26 >> 16) & 0x3ffu);
}

void la_w(struct code *c, unsigned long w)
{
    code_u32(c, w & 0xffffffffUL);
}

/* ---- arithmetic and logic ------------------------------------------- */

/* The register forms: the .d (or only) opcode, and the .w one or 0. */
static const struct { unsigned long d, w; } alu_op[LA_NALU] = {
    [LA_ADD]     = { 0x00108000UL, 0x00100000UL },
    [LA_SUB]     = { 0x00118000UL, 0x00110000UL },
    [LA_SLT]     = { 0x00120000UL, 0 },
    [LA_SLTU]    = { 0x00128000UL, 0 },
    [LA_AND]     = { 0x00148000UL, 0 },
    [LA_OR]      = { 0x00150000UL, 0 },
    [LA_XOR]     = { 0x00158000UL, 0 },
    [LA_NOR]     = { 0x00140000UL, 0 },
    [LA_ANDN]    = { 0x00168000UL, 0 },
    [LA_ORN]     = { 0x00160000UL, 0 },
    [LA_SLL]     = { 0x00188000UL, 0x00170000UL },
    [LA_SRL]     = { 0x00190000UL, 0x00178000UL },
    [LA_SRA]     = { 0x00198000UL, 0x00180000UL },
    [LA_ROTR]    = { 0x001b8000UL, 0x001b0000UL },
    [LA_MASKEQZ] = { 0x00130000UL, 0 },
    [LA_MASKNEZ] = { 0x00138000UL, 0 },
    [LA_MUL]     = { 0x001d8000UL, 0x001c0000UL },
    [LA_MULH]    = { 0x001e0000UL, 0x001c8000UL },
    [LA_MULHU]   = { 0x001e8000UL, 0x001d0000UL },
    [LA_DIV]     = { 0x00220000UL, 0x00200000UL },
    [LA_DIVU]    = { 0x00230000UL, 0x00210000UL },
    [LA_MOD]     = { 0x00228000UL, 0x00208000UL },
    [LA_MODU]    = { 0x00238000UL, 0x00218000UL },
};

void la_alu(struct code *c, int op, int rd, int rj, int rk, int w)
{
    if (op < 0 || op >= LA_NALU)
        internal_error("loongarch: alu op %d is not one of the %d", op,
                       LA_NALU);
    if (w && !alu_op[op].w)
        internal_error("loongarch: alu op %d has no 32-bit form", op);
    la_w(c, la_enc_3r(w ? alu_op[op].w : alu_op[op].d, rd, rj, rk));
}

void la_alu_imm(struct code *c, int op, int rd, int rj, long long imm, int w)
{
    unsigned long opc;
    if (w && op != LA_ADD)
        internal_error("loongarch: only addi has a 32-bit immediate form");
    switch (op) {
    case LA_ADD:
        need_s(imm, 12, w ? "addi.w" : "addi.d");
        opc = w ? 0x02800000UL : 0x02c00000UL;
        break;
    case LA_SLT:
        need_s(imm, 12, "slti");
        opc = 0x02000000UL;
        break;
    case LA_SLTU:
        need_s(imm, 12, "sltui");
        opc = 0x02400000UL;
        break;
    case LA_AND:
        need_u(imm, 12, "andi");
        opc = 0x03400000UL;
        break;
    case LA_OR:
        need_u(imm, 12, "ori");
        opc = 0x03800000UL;
        break;
    case LA_XOR:
        need_u(imm, 12, "xori");
        opc = 0x03c00000UL;
        break;
    default:
        internal_error("loongarch: alu op %d has no immediate form", op);
        return;
    }
    la_w(c, la_enc_2ri12(opc, rd, rj, (unsigned)(imm & 0xfff)));
}

void la_shift_imm(struct code *c, int op, int rd, int rj, int amt, int w)
{
    unsigned long opc;
    switch (op) {
    case LA_SLL:  opc = w ? 0x00408000UL : 0x00410000UL; break;
    case LA_SRL:  opc = w ? 0x00448000UL : 0x00450000UL; break;
    case LA_SRA:  opc = w ? 0x00488000UL : 0x00490000UL; break;
    case LA_ROTR: opc = w ? 0x004c8000UL : 0x004d0000UL; break;
    default:
        internal_error("loongarch: %d is not a shift", op);
        return;
    }
    need_u(amt, w ? 5 : 6, "shift amount");
    need_reg(rd); need_reg(rj);
    /* ui5 or ui6 at 10, the rest of the opcode above it */
    la_w(c, opc | ((unsigned long)amt << 10) |
            ((unsigned long)rj << 5) | (unsigned long)rd);
}

void la_mv(struct code *c, int rd, int rj)
{
    la_alu(c, LA_OR, rd, rj, LA_ZERO, 0);
}

void la_nop(struct code *c)
{
    la_w(c, 0x03400000UL);              /* andi zero, zero, 0 */
}

void la_ext(struct code *c, int rd, int rj, int size)
{
    if (size != 1 && size != 2)
        internal_error("loongarch: there is no %d-byte ext.w", size);
    la_w(c, la_enc_2r(size == 1 ? 0x00005c00UL : 0x00005800UL, rd, rj));
}

void la_bstrpick(struct code *c, int rd, int rj, int msb, int lsb, int d)
{
    int bits = d ? 6 : 5;
    need_reg(rd); need_reg(rj);
    need_u(msb, bits, "bstrpick msb");
    need_u(lsb, bits, "bstrpick lsb");
    if (msb < lsb)
        internal_error("loongarch: bstrpick %d:%d is an empty field", msb, lsb);
    if (d)
        la_w(c, 0x00c00000UL | ((unsigned long)msb << 16) |
                ((unsigned long)lsb << 10) | ((unsigned long)rj << 5) |
                (unsigned long)rd);
    else
        la_w(c, 0x00608000UL | ((unsigned long)msb << 16) |
                ((unsigned long)lsb << 10) | ((unsigned long)rj << 5) |
                (unsigned long)rd);
}

void la_alsl(struct code *c, int rd, int rj, int rk, int sa, int d)
{
    if (sa < 1 || sa > 4)
        internal_error("loongarch: alsl shifts by 1..4, not %d", sa);
    /* the field holds sa - 1 */
    la_w(c, la_enc_3r(d ? 0x002c0000UL : 0x00040000UL, rd, rj, rk) |
            ((unsigned long)(sa - 1) << 15));
}

void la_revb(struct code *c, int op, int rd, int rj)
{
    static const unsigned long opc[] = {
        0x00003000UL, 0x00003400UL, 0x00003800UL, 0x00003c00UL
    };
    if (op < LA_REVB_2H || op > LA_REVB_D)
        internal_error("loongarch: revb form %d", op);
    la_w(c, la_enc_2r(opc[op], rd, rj));
}

/* ---- constants and addresses ------------------------------------------ */

void la_lu12i(struct code *c, int rd, long si20)
{
    need_s(si20, 20, "lu12i.w");
    la_w(c, la_enc_1ri20(0x14000000UL, rd, (unsigned)si20 & 0xfffffu));
}

void la_lu32i(struct code *c, int rd, long si20)
{
    need_s(si20, 20, "lu32i.d");
    la_w(c, la_enc_1ri20(0x16000000UL, rd, (unsigned)si20 & 0xfffffu));
}

void la_lu52i(struct code *c, int rd, int rj, long si12)
{
    need_s(si12, 12, "lu52i.d");
    la_w(c, la_enc_2ri12(0x03000000UL, rd, rj, (unsigned)si12 & 0xfffu));
}

void la_pcrel(struct code *c, int op, int rd, long si20)
{
    static const unsigned long opc[] = {
        0x18000000UL, 0x1a000000UL, 0x1c000000UL, 0x1e000000UL
    };
    if (op < LA_PCADDI || op > LA_PCADDU18I)
        internal_error("loongarch: pc-relative form %d", op);
    need_s(si20, 20, "pc-relative immediate");
    la_w(c, la_enc_1ri20(opc[op], rd, (unsigned)si20 & 0xfffffu));
}

static long long sext(unsigned long long v, int bits)
{
    unsigned long long m = 1ULL << (bits - 1);
    v &= (1ULL << bits) - 1;
    return (long long)((v ^ m) - m);
}

/* The sequence, emitted or only counted. LLVM's LoongArchMatInt, step
 * for step: the low 32 bits (sign-extended into the register), then
 * bits 51:32 with lu32i.d -- which sign-extends bit 51 -- then 63:52
 * with lu52i.d, each only where what is below does not already give it.
 * A value that is all zeros below bit 52 is one lu52i.d from zero. */
static int li_emit(struct code *c, int rd, long long v, int emit)
{
    unsigned long long u = (unsigned long long)v;
    long long hi12 = sext(u >> 52, 12);
    long long higher20 = sext(u >> 32, 20);
    long long hi20 = sext(u >> 12, 20);
    long long lo12u = (long long)(u & 0xfff);
    int n = 0;

    if (hi12 != 0 && sext(u, 52) == 0) {
        if (emit) la_lu52i(c, rd, LA_ZERO, (long)hi12);
        return 4;
    }
    if (hi20 == 0) {
        if (emit) la_alu_imm(c, LA_OR, rd, LA_ZERO, lo12u, 0);
        n += 4;
    } else if ((lo12u >> 11 ? -1 : 0) == hi20) {
        if (emit) la_alu_imm(c, LA_ADD, rd, LA_ZERO, sext(u, 12), 1);
        n += 4;
    } else {
        if (emit) la_lu12i(c, rd, (long)hi20);
        n += 4;
        if (lo12u) {
            if (emit) la_alu_imm(c, LA_OR, rd, rd, lo12u, 0);
            n += 4;
        }
    }
    if ((hi20 < 0 ? -1 : 0) != higher20) {
        if (emit) la_lu32i(c, rd, (long)higher20);
        n += 4;
    }
    if ((higher20 < 0 ? -1 : 0) != hi12) {
        if (emit) la_lu52i(c, rd, rd, (long)hi12);
        n += 4;
    }
    return n;
}

void la_li(struct code *c, int rd, long long v)
{
    (void)li_emit(c, rd, v, 1);
}

int la_li_len(long long v)
{
    return li_emit(NULL, 0, v, 0);
}

/* ---- memory ---------------------------------------------------------- */

void la_load(struct code *c, int rd, int rj, int off, int size, int sign)
{
    unsigned long opc;
    switch (size) {
    case 1: opc = sign ? 0x28000000UL : 0x2a000000UL; break;
    case 2: opc = sign ? 0x28400000UL : 0x2a400000UL; break;
    case 4: opc = sign ? 0x28800000UL : 0x2a800000UL; break;
    case 8: opc = 0x28c00000UL; break;
    default:
        internal_error("loongarch: no %d-byte load", size);
        return;
    }
    need_s(off, 12, "load offset");
    la_w(c, la_enc_2ri12(opc, rd, rj, (unsigned)off & 0xfffu));
}

void la_store(struct code *c, int rd, int rj, int off, int size)
{
    unsigned long opc;
    switch (size) {
    case 1: opc = 0x29000000UL; break;
    case 2: opc = 0x29400000UL; break;
    case 4: opc = 0x29800000UL; break;
    case 8: opc = 0x29c00000UL; break;
    default:
        internal_error("loongarch: no %d-byte store", size);
        return;
    }
    need_s(off, 12, "store offset");
    la_w(c, la_enc_2ri12(opc, rd, rj, (unsigned)off & 0xfffu));
}

/* ---- control flow ----------------------------------------------------- */

static const unsigned long br_op[] = {
    [LA_BEQ] = 0x58000000UL, [LA_BNE] = 0x5c000000UL,
    [LA_BLT] = 0x60000000UL, [LA_BGE] = 0x64000000UL,
    [LA_BLTU] = 0x68000000UL, [LA_BGEU] = 0x6c000000UL,
    [LA_BEQZ] = 0x40000000UL, [LA_BNEZ] = 0x44000000UL
};

int la_branch_reaches(int cond, long off)
{
    if (off & 3)
        return 0;
    return cond == LA_BEQZ || cond == LA_BNEZ ? la_fits(off >> 2, 21)
                                              : la_fits(off >> 2, 16);
}

unsigned long la_enc_branch(int cond, int rj, int rd, long off)
{
    if (cond < LA_BEQ || cond > LA_BNEZ)
        internal_error("loongarch: branch condition %d", cond);
    if (off & 3)
        internal_error("loongarch: branch offset %ld is not a multiple of 4",
                       off);
    if (cond == LA_BEQZ || cond == LA_BNEZ) {
        need_s(off >> 2, 21, "beqz/bnez offset");
        if (rd != LA_ZERO)
            internal_error("loongarch: beqz/bnez compare one register");
        return la_enc_1ri21(br_op[cond], rj, (unsigned)(off >> 2) & 0x1fffffu);
    }
    need_s(off >> 2, 16, "branch offset");
    return la_enc_2ri16(br_op[cond], rd, rj, (unsigned)(off >> 2) & 0xffffu);
}

int la_b_placeholder(struct code *c, int cond, int rj, int rd)
{
    int at = c->len;
    la_w(c, la_enc_branch(cond, rj, rd, 0));
    return at;
}

static unsigned long word_at(const struct code *c, int at)
{
    return (unsigned long)c->p[at] | ((unsigned long)c->p[at + 1] << 8) |
           ((unsigned long)c->p[at + 2] << 16) |
           ((unsigned long)c->p[at + 3] << 24);
}

/* Which branch is at `at` is read back from its opcode, so a patch keeps
 * the condition and the registers and replaces only the offset. */
int la_patch_b(struct code *c, int at, int target)
{
    unsigned long w = word_at(c, at), op = w & 0xfc000000UL;
    long off = (long)target - at;
    if (off & 3)
        return 0;
    if (op == 0x40000000UL || op == 0x44000000UL) {          /* beqz, bnez */
        if (!la_fits(off >> 2, 21))
            return 0;
        code_patch32(c, at, la_enc_1ri21(op, (int)((w >> 5) & 31),
                                         (unsigned)(off >> 2) & 0x1fffffu));
        return 1;
    }
    if (op < 0x58000000UL || op > 0x6c000000UL)
        internal_error("loongarch: patching 0x%08lx, which is not a branch", w);
    if (!la_fits(off >> 2, 16))
        return 0;
    code_patch32(c, at, la_enc_2ri16(op, (int)(w & 31), (int)((w >> 5) & 31),
                                     (unsigned)(off >> 2) & 0xffffu));
    return 1;
}

unsigned long la_enc_j(int link, long off)
{
    if (off & 3)
        internal_error("loongarch: jump offset %ld is not a multiple of 4",
                       off);
    need_s(off >> 2, 26, "b/bl offset");
    return la_enc_i26(link ? 0x54000000UL : 0x50000000UL,
                      (unsigned)(off >> 2) & 0x3ffffffu);
}

int la_j_placeholder(struct code *c, int link)
{
    int at = c->len;
    la_w(c, la_enc_j(link, 0));
    return at;
}

int la_patch_j(struct code *c, int at, int target)
{
    unsigned long w = word_at(c, at);
    long off = (long)target - at;
    if (off & 3 || !la_fits(off >> 2, 26))
        return 0;
    if ((w & 0xfc000000UL) != 0x50000000UL && (w & 0xfc000000UL) != 0x54000000UL)
        internal_error("loongarch: patching 0x%08lx, which is not b or bl", w);
    code_patch32(c, at, la_enc_j((w & 0xfc000000UL) == 0x54000000UL, off));
    return 1;
}

void la_jirl(struct code *c, int rd, int rj, long off)
{
    if (off & 3)
        internal_error("loongarch: jirl offset %ld is not a multiple of 4",
                       off);
    need_s(off >> 2, 16, "jirl offset");
    la_w(c, la_enc_2ri16(0x4c000000UL, rd, rj, (unsigned)(off >> 2) & 0xffffu));
}

void la_ret(struct code *c)
{
    la_jirl(c, LA_ZERO, LA_RA, 0);
}

/* ---- atomics and barriers --------------------------------------------- */

static void llsc(struct code *c, unsigned long op, int rd, int rj, int off)
{
    if (off & 3)
        internal_error("loongarch: ll/sc offset %d is not a multiple of 4",
                       off);
    need_s(off >> 2, 14, "ll/sc offset");
    la_w(c, la_enc_2ri14(op, rd, rj, (unsigned)(off >> 2) & 0x3fffu));
}

void la_ll(struct code *c, int rd, int rj, int off, int d)
{
    llsc(c, d ? 0x22000000UL : 0x20000000UL, rd, rj, off);
}

void la_sc(struct code *c, int rd, int rj, int off, int d)
{
    llsc(c, d ? 0x23000000UL : 0x21000000UL, rd, rj, off);
}

void la_am(struct code *c, int op, int rd, int rk, int rj, int d)
{
    static const unsigned long opc[] = {
        [LA_AMSWAP] = 0x38690000UL, [LA_AMADD] = 0x386a0000UL,
        [LA_AMAND] = 0x386b0000UL, [LA_AMOR] = 0x386c0000UL,
        [LA_AMXOR] = 0x386d0000UL
    };
    if (op < LA_AMSWAP || op > LA_AMXOR)
        internal_error("loongarch: am* form %d", op);
    if (rd != LA_ZERO && (rd == rj || rd == rk))
        internal_error("loongarch: an am* instruction's rd (%d) is one of its "
                       "sources", rd);
    la_w(c, la_enc_3r(opc[op] | (d ? 0x8000UL : 0), rd, rj, rk));
}

void la_dbar(struct code *c, int hint)
{
    need_u(hint, 15, "dbar hint");
    la_w(c, 0x38720000UL | (unsigned long)hint);
}

void la_break(struct code *c, int code)
{
    need_u(code, 15, "break code");
    la_w(c, 0x002a0000UL | (unsigned long)code);
}

/* ---- what only the assembler emits (see emit.h) ------------------------ */

const struct la_raw la_raw_insns[] = {
    { "clo.w", LAF_2R, 0x00001000UL, 0 }, { "clz.w", LAF_2R, 0x00001400UL, 0 },
    { "cto.w", LAF_2R, 0x00001800UL, 0 }, { "ctz.w", LAF_2R, 0x00001c00UL, 0 },
    { "clo.d", LAF_2R, 0x00002000UL, 0 }, { "clz.d", LAF_2R, 0x00002400UL, 0 },
    { "cto.d", LAF_2R, 0x00002800UL, 0 }, { "ctz.d", LAF_2R, 0x00002c00UL, 0 },
    { "bitrev.4b", LAF_2R, 0x00004800UL, 0 },
    { "bitrev.8b", LAF_2R, 0x00004c00UL, 0 },
    { "bitrev.w", LAF_2R, 0x00005000UL, 0 },
    { "bitrev.d", LAF_2R, 0x00005400UL, 0 },
    { "cpucfg", LAF_2R, 0x00006c00UL, 0 },
    { "rdtimel.w", LAF_2R, 0x00006000UL, 0 },
    { "rdtimeh.w", LAF_2R, 0x00006400UL, 0 },
    { "rdtime.d", LAF_2R, 0x00006800UL, 0 },
    { "iocsrrd.b", LAF_2R, 0x06480000UL, 0 }, { "iocsrrd.h", LAF_2R, 0x06480400UL, 0 },
    { "iocsrrd.w", LAF_2R, 0x06480800UL, 0 }, { "iocsrrd.d", LAF_2R, 0x06480c00UL, 0 },
    { "iocsrwr.b", LAF_2R, 0x06481000UL, 0 }, { "iocsrwr.h", LAF_2R, 0x06481400UL, 0 },
    { "iocsrwr.w", LAF_2R, 0x06481800UL, 0 }, { "iocsrwr.d", LAF_2R, 0x06481c00UL, 0 },
    { "ldx.b", LAF_3R, 0x38000000UL, 0 }, { "ldx.h", LAF_3R, 0x38040000UL, 0 },
    { "ldx.w", LAF_3R, 0x38080000UL, 0 }, { "ldx.d", LAF_3R, 0x380c0000UL, 0 },
    { "ldx.bu", LAF_3R, 0x38200000UL, 0 }, { "ldx.hu", LAF_3R, 0x38240000UL, 0 },
    { "ldx.wu", LAF_3R, 0x38280000UL, 0 },
    { "stx.b", LAF_3R, 0x38100000UL, 0 }, { "stx.h", LAF_3R, 0x38140000UL, 0 },
    { "stx.w", LAF_3R, 0x38180000UL, 0 }, { "stx.d", LAF_3R, 0x381c0000UL, 0 },
    { "mulw.d.w", LAF_3R, 0x001f0000UL, 0 },
    { "mulw.d.wu", LAF_3R, 0x001f8000UL, 0 },
    { "ldptr.w", LAF_PTR, 0x24000000UL, 0 }, { "stptr.w", LAF_PTR, 0x25000000UL, 0 },
    { "ldptr.d", LAF_PTR, 0x26000000UL, 1 }, { "stptr.d", LAF_PTR, 0x27000000UL, 1 },
    { "amswap.w", LAF_AM, 0x38600000UL, 0 }, { "amswap.d", LAF_AM, 0x38608000UL, 1 },
    { "amadd.w", LAF_AM, 0x38610000UL, 0 }, { "amadd.d", LAF_AM, 0x38618000UL, 1 },
    { "amand.w", LAF_AM, 0x38620000UL, 0 }, { "amand.d", LAF_AM, 0x38628000UL, 1 },
    { "amor.w", LAF_AM, 0x38630000UL, 0 }, { "amor.d", LAF_AM, 0x38638000UL, 1 },
    { "amxor.w", LAF_AM, 0x38640000UL, 0 }, { "amxor.d", LAF_AM, 0x38648000UL, 1 },
    { "ammax.w", LAF_AM, 0x38650000UL, 0 }, { "ammax.d", LAF_AM, 0x38658000UL, 1 },
    { "ammin.w", LAF_AM, 0x38660000UL, 0 }, { "ammin.d", LAF_AM, 0x38668000UL, 1 },
    { "ammax.wu", LAF_AM, 0x38670000UL, 0 }, { "ammax.du", LAF_AM, 0x38678000UL, 1 },
    { "ammin.wu", LAF_AM, 0x38680000UL, 0 }, { "ammin.du", LAF_AM, 0x38688000UL, 1 },
    { "ammax_db.w", LAF_AM, 0x386e0000UL, 0 }, { "ammax_db.d", LAF_AM, 0x386e8000UL, 1 },
    { "ammin_db.w", LAF_AM, 0x386f0000UL, 0 }, { "ammin_db.d", LAF_AM, 0x386f8000UL, 1 },
    { "ammax_db.wu", LAF_AM, 0x38700000UL, 0 }, { "ammax_db.du", LAF_AM, 0x38708000UL, 1 },
    { "ammin_db.wu", LAF_AM, 0x38710000UL, 0 }, { "ammin_db.du", LAF_AM, 0x38718000UL, 1 },
    { "csrrd", LAF_CSRRD, 0x04000000UL, 0 },
    { "csrwr", LAF_CSRWR, 0x04000020UL, 0 },
    { "csrxchg", LAF_CSRXCHG, 0x04000000UL, 0 },
    { "syscall", LAF_CODE15, 0x002b0000UL, 0 },
    { "idle", LAF_CODE15, 0x06488000UL, 0 },
    { "ibar", LAF_CODE15, 0x38728000UL, 0 },
    { "ertn", LAF_NONE, 0x06483800UL, 0 },
    { "bstrins.w", LAF_BSTRINS, 0x00600000UL, 0 },
    { "bstrins.d", LAF_BSTRINS, 0x00800000UL, 1 },
    { NULL, 0, 0, 0 }
};
