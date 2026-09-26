/* RISC-V instruction encoding (D-016). See emit.h for why this is short. */
#include "emit.h"

#include "../../driver/util.h"

const int rv_argreg[RV_NARGREG] = {
    RV_A0, RV_A1, RV_A2, RV_A3, RV_A4, RV_A5, RV_A6, RV_A7
};

/* The seven-bit opcode field. Named as the ISA manual names them, because
 * that is the only place they can be checked against. */
enum {
    OP_LOAD   = 0x03, OP_IMM    = 0x13, OP_AUIPC  = 0x17, OP_IMM32 = 0x1b,
    OP_STORE  = 0x23, OP_OP     = 0x33, OP_LUI    = 0x37, OP_OP32  = 0x3b,
    OP_BRANCH = 0x63, OP_JALR   = 0x67, OP_JAL    = 0x6f, OP_SYSTEM = 0x73
};

int rv_fits(long long v, int bits)
{
    long long lo = -(1LL << (bits - 1)), hi = (1LL << (bits - 1)) - 1;
    return v >= lo && v <= hi;
}

/* Every immediate field in this instruction set is signed, and a value
 * that does not fit must never be truncated: the result assembles, links,
 * and branches into the middle of another function. The encoders abort
 * instead, so a codegen that forgot to widen a sequence fails at the
 * moment it emits rather than at run time. */
static void need(long long v, int bits, const char *what)
{
    if (!rv_fits(v, bits))
        internal_error("riscv: %s: %lld does not fit in %d signed bits", what, v, bits);
}

static void need_reg(int r)
{
    if (r < 0 || r > 31)
        internal_error("riscv: register %d is not x0-x31", r);
}

unsigned long rv_enc_r(int op, int rd, int f3, int rs1, int rs2, int f7)
{
    need_reg(rd); need_reg(rs1); need_reg(rs2);
    return (unsigned long)op | ((unsigned long)rd << 7) |
           ((unsigned long)f3 << 12) | ((unsigned long)rs1 << 15) |
           ((unsigned long)rs2 << 20) | ((unsigned long)f7 << 25);
}

unsigned long rv_enc_i(int op, int rd, int f3, int rs1, int imm)
{
    need_reg(rd); need_reg(rs1);
    need(imm, 12, "I-type immediate");
    return (unsigned long)op | ((unsigned long)rd << 7) |
           ((unsigned long)f3 << 12) | ((unsigned long)rs1 << 15) |
           (((unsigned long)imm & 0xfff) << 20);
}

/* S-type: the same 12 bits as I-type, split so that rs1/rs2/funct3 stay
 * where every other format has them. That is the whole reason the format
 * exists, and the reason it is packed here rather than at each caller. */
unsigned long rv_enc_s(int op, int f3, int rs1, int rs2, int imm)
{
    need_reg(rs1); need_reg(rs2);
    need(imm, 12, "S-type offset");
    unsigned long u = (unsigned long)imm & 0xfff;
    return (unsigned long)op | ((u & 0x1f) << 7) | ((unsigned long)f3 << 12) |
           ((unsigned long)rs1 << 15) | ((unsigned long)rs2 << 20) |
           ((u >> 5) << 25);
}

/* B-type: a 13-bit signed displacement with bit 0 always zero, so only
 * bits 12:1 are stored -- and they are stored out of order (12, 10:5,
 * 4:1, 11). The low bit is not merely ignored: an odd displacement is a
 * bug, so it is rejected. */
unsigned long rv_enc_b(int op, int f3, int rs1, int rs2, int imm)
{
    need_reg(rs1); need_reg(rs2);
    if (imm & 1)
        internal_error("riscv: branch displacement %d is odd", imm);
    need(imm, 13, "branch displacement");
    unsigned long u = (unsigned long)imm;
    return (unsigned long)op | ((unsigned long)f3 << 12) |
           ((unsigned long)rs1 << 15) | ((unsigned long)rs2 << 20) |
           (((u >> 11) & 1) << 7) | (((u >> 1) & 0xf) << 8) |
           (((u >> 5) & 0x3f) << 25) | (((u >> 12) & 1) << 31);
}

/* U-type takes the HI20 FIELD, not an address: the caller has already
 * done the +0x800 rounding that pairs it with a sign-extended LO12. */
unsigned long rv_enc_u(int op, int rd, long hi20)
{
    need_reg(rd);
    return (unsigned long)op | ((unsigned long)rd << 7) |
           (((unsigned long)hi20 & 0xfffff) << 12);
}

/* J-type: 21 bits, low bit implicit, stored as (20, 10:1, 11, 19:12). */
unsigned long rv_enc_j(int op, int rd, int imm)
{
    need_reg(rd);
    if (imm & 1)
        internal_error("riscv: jump displacement %d is odd", imm);
    need(imm, 21, "jump displacement");
    unsigned long u = (unsigned long)imm;
    return (unsigned long)op | ((unsigned long)rd << 7) |
           (((u >> 12) & 0xff) << 12) | (((u >> 11) & 1) << 20) |
           (((u >> 1) & 0x3ff) << 21) | (((u >> 20) & 1) << 31);
}

/* ---- moves and constants -------------------------------------------- */

void rv_mv(struct code *c, int rd, int rs)
{
    code_u32(c, rv_enc_i(OP_IMM, rd, 0, rs, 0));
}

void rv_lui(struct code *c, int rd, long hi20)
{
    code_u32(c, rv_enc_u(OP_LUI, rd, hi20));
}

void rv_auipc(struct code *c, int rd, long hi20)
{
    code_u32(c, rv_enc_u(OP_AUIPC, rd, hi20));
}

/* THE +0x800. `lui`/`auipc` supply bits 31:12 and the instruction beside
 * them supplies a SIGN-EXTENDED low 12. So when bit 11 of the value is
 * set, the low half contributes -4096..-1 and the high half must be one
 * larger to compensate. Every RISC-V toolchain has this line and every
 * one that omits it is wrong by 4096 for half of all addresses. */
static long hi20_of(long long v) { return (long)(((v + 0x800) >> 12) & 0xfffff); }
static int lo12_of(long long v) { return (int)(((v & 0xfff) ^ 0x800) - 0x800); }

/* The shift-and-add chain for a value wider than 32 bits. Peels a signed
 * low 12 off the bottom, recurses on what is left, and shifts it back --
 * so the recursion depth is bounded by 64/12 and the result is exact for
 * every 64-bit value. */
static int li_emit(struct code *c, int rd, long long v, int xlen, int emit)
{
    if (rv_fits(v, 12)) {
        if (emit) code_u32(c, rv_enc_i(OP_IMM, rd, 0, RV_ZERO, (int)v));
        return 4;
    }
    if (rv_fits(v, 32)) {
        int lo = lo12_of(v);
        int n = 4;
        if (emit) code_u32(c, rv_enc_u(OP_LUI, rd, hi20_of(v)));
        if (lo) {
            /* ADDIW at RV64 and ADDI at RV32, and they are not the same
             * instruction -- addiw does not exist at RV32 at all. At RV64
             * it is required: `lui` sign-extends bit 31 into the top half,
             * so lui(0x80000)+addi(-1) for 0x7fffffff leaves
             * 0xffffffff7fffffff. addiw re-narrows the sum to 32 bits and
             * sign-extends it, which is the answer. */
            if (emit)
                code_u32(c, rv_enc_i(xlen == 64 ? OP_IMM32 : OP_IMM,
                                     rd, 0, rd, lo));
            n += 4;
        }
        return n;
    }
    int lo = lo12_of(v);
    long long hi = (v - lo) >> 12;
    int n = li_emit(c, rd, hi, xlen, emit);
    if (emit) code_u32(c, rv_enc_r(OP_IMM, rd, 1, rd, 12, 0)); /* slli rd,rd,12 */
    n += 4;
    if (lo) {
        if (emit) code_u32(c, rv_enc_i(OP_IMM, rd, 0, rd, lo));
        n += 4;
    }
    return n;
}

/* A register holds XLEN bits, so at RV32 the caller's `long long` must
 * name a value that fits in 32 -- as either a signed or an unsigned one,
 * since 0xffffffff and -1 are the same register contents. Narrowing is
 * done here, definedly, rather than by a cast whose result above INT_MAX
 * is implementation-defined. */
static long long narrow32(long long v)
{
    unsigned int u = (unsigned int)v;
    return (u & 0x80000000u) ? (long long)u - 0x100000000LL : (long long)u;
}

void rv_li(struct code *c, int rd, long long v, int xlen)
{
    if (xlen == 32) {
        if (v < -2147483648LL || v > 4294967295LL)
            internal_error("riscv: %lld does not fit in a 32-bit register", v);
        v = narrow32(v);
    }
    (void)li_emit(c, rd, v, xlen, 1);
}

int rv_li_len(long long v, int xlen)
{
    if (xlen == 32) v = narrow32(v);
    return li_emit(NULL, 0, v, xlen, 0);
}

/* ---- arithmetic and logic ------------------------------------------- */

/* funct3 and the funct7 bit, per operation, in ONE table. The word forms
 * differ from these only in the opcode, which is why `w` is a parameter
 * and not a second table. */
static const struct { int f3, alt; } alu_enc[] = {
    [RV_ADD]  = { 0, 0 }, [RV_SUB]  = { 0, 1 }, [RV_SLL] = { 1, 0 },
    [RV_SLT]  = { 2, 0 }, [RV_SLTU] = { 3, 0 }, [RV_XOR] = { 4, 0 },
    [RV_SRL]  = { 5, 0 }, [RV_SRA]  = { 5, 1 }, [RV_OR]  = { 6, 0 },
    [RV_AND]  = { 7, 0 }
};

/* The five operations that have a 32-bit form at RV64. `slt`, `sltu`,
 * `xor`, `or` and `and` have none -- their results do not depend on the
 * width -- and asking for one is a bug rather than a fallback. */
static int has_word_form(int op)
{
    return op == RV_ADD || op == RV_SUB || op == RV_SLL ||
           op == RV_SRL || op == RV_SRA;
}

void rv_alu(struct code *c, int op, int rd, int rs1, int rs2, int w)
{
    if (op < 0 || op > RV_AND)
        internal_error("riscv: alu op %d is not one of the ten", op);
    if (w && !has_word_form(op))
        internal_error("riscv: alu op %d has no 32-bit form", op);
    code_u32(c, rv_enc_r(w ? OP_OP32 : OP_OP, rd, alu_enc[op].f3, rs1, rs2,
                         alu_enc[op].alt ? 0x20 : 0));
}

void rv_alu_imm(struct code *c, int op, int rd, int rs1, int imm, int w)
{
    if (op == RV_SUB)
        internal_error("riscv: sub has no immediate form; add a negative");
    if (op == RV_SLL || op == RV_SRL || op == RV_SRA)
        internal_error("riscv: a shift immediate is not an I-type one; "
              "use rv_shift_imm");
    if (op < 0 || op > RV_AND)
        internal_error("riscv: alu op %d is not one of the ten", op);
    if (w && !has_word_form(op))
        internal_error("riscv: alu op %d has no 32-bit form", op);
    code_u32(c, rv_enc_i(w ? OP_IMM32 : OP_IMM, rd, alu_enc[op].f3, rs1, imm));
}

/* A shift immediate is NOT a signed 12-bit field. It is a shift AMOUNT in
 * the low bits with funct7 above it, and how wide it may be depends on
 * both the register width and whether this is a word form: 6 bits for a
 * full-width shift at RV64, 5 everywhere else. Sent through the I-type
 * packer, a shift of 32 at RV32 would encode as a legal-looking `srai` of
 * 0 with a funct7 bit set -- accepted by the assembler and wrong. This is
 * why the shifts are a function of their own and why it takes xlen. */
void rv_shift_imm(struct code *c, int op, int rd, int rs1, int amt,
                  int w, int xlen)
{
    if (op != RV_SLL && op != RV_SRL && op != RV_SRA)
        internal_error("riscv: %d is not a shift", op);
    if (w && xlen != 64)
        internal_error("riscv: there is no 32-bit shift form at RV32");
    int bits = (xlen == 64 && !w) ? 6 : 5;
    if (amt < 0 || amt >= (1 << bits))
        internal_error("riscv: shift amount %d is not 0..%d here", amt, (1 << bits) - 1);
    code_u32(c, rv_enc_r(w ? OP_IMM32 : OP_IMM, rd, alu_enc[op].f3, rs1,
                         amt & 0x1f,
                         (alu_enc[op].alt ? 0x20 : 0) | ((amt >> 5) & 1)));
}

static const int muldiv_f3[] = {
    [RV_MUL] = 0, [RV_MULH] = 1, [RV_MULHSU] = 2, [RV_MULHU] = 3,
    [RV_DIV] = 4, [RV_DIVU] = 5, [RV_REM] = 6, [RV_REMU] = 7
};

void rv_muldiv(struct code *c, int op, int rd, int rs1, int rs2, int w)
{
    if (op < 0 || op > RV_REMU)
        internal_error("riscv: muldiv op %d is not one of the eight", op);
    if (w && (op == RV_MULH || op == RV_MULHSU || op == RV_MULHU))
        internal_error("riscv: the high-half multiplies have no 32-bit form");
    code_u32(c, rv_enc_r(w ? OP_OP32 : OP_OP, rd, muldiv_f3[op], rs1, rs2, 1));
}

/* ---- memory ---------------------------------------------------------- */

/* The loads are where the two widths differ in what EXISTS, not merely
 * in what they mean, so this takes xlen and checks rather than trusting
 * the caller. `lwu` and `ld` are RV64-only; at RV32 a four-byte load IS
 * the whole register, so there is no unsigned form to ask for and `lw`
 * is the answer for both signednesses. Emitting `lwu` at RV32 assembles
 * into a real 32-bit word and traps as an illegal instruction the first
 * time a struct argument is copied. */
void rv_load(struct code *c, int rd, int rs1, int off, int size, int sign,
             int xlen)
{
    int f3;
    switch (size) {
    case 1: f3 = sign ? 0 : 4; break;
    case 2: f3 = sign ? 1 : 5; break;
    case 4: f3 = (sign || xlen == 32) ? 2 : 6; break;
    case 8:
        if (xlen != 64)
            internal_error("riscv: there is no eight-byte load at RV32");
        f3 = 3;
        break;
    default: internal_error("riscv: no %d-byte load", size);
    }
    code_u32(c, rv_enc_i(OP_LOAD, rd, f3, rs1, off));
}

void rv_store(struct code *c, int rs2, int rs1, int off, int size, int xlen)
{
    int f3;
    switch (size) {
    case 1: f3 = 0; break;
    case 2: f3 = 1; break;
    case 4: f3 = 2; break;
    case 8:
        if (xlen != 64)
            internal_error("riscv: there is no eight-byte store at RV32");
        f3 = 3;
        break;
    default: internal_error("riscv: no %d-byte store", size);
    }
    code_u32(c, rv_enc_s(OP_STORE, f3, rs1, rs2, off));
}

/* ---- control flow ----------------------------------------------------- */

int rv_b_placeholder(struct code *c, int cond, int rs1, int rs2)
{
    int at = c->len;
    code_u32(c, rv_enc_b(OP_BRANCH, cond, rs1, rs2, 0));
    return at;
}

void rv_patch_b(struct code *c, int at, int target)
{
    unsigned long w = (unsigned long)c->p[at] | ((unsigned long)c->p[at+1] << 8) |
                      ((unsigned long)c->p[at+2] << 16) |
                      ((unsigned long)c->p[at+3] << 24);
    /* Keep funct3/rs1/rs2 and replace only the displacement bits, so a
     * patch cannot quietly change which branch this is. */
    w &= ~0xfe000f80UL;
    w |= rv_enc_b(0, 0, 0, 0, target - at);
    code_patch32(c, at, w);
}

int rv_j_placeholder(struct code *c, int rd)
{
    int at = c->len;
    code_u32(c, rv_enc_j(OP_JAL, rd, 0));
    return at;
}

void rv_patch_j(struct code *c, int at, int target)
{
    unsigned long w = (unsigned long)c->p[at] | ((unsigned long)c->p[at+1] << 8) |
                      ((unsigned long)c->p[at+2] << 16) |
                      ((unsigned long)c->p[at+3] << 24);
    w &= ~0xfffff000UL;
    w |= rv_enc_j(0, 0, target - at);
    code_patch32(c, at, w);
}

void rv_jalr(struct code *c, int rd, int rs1, int off)
{
    code_u32(c, rv_enc_i(OP_JALR, rd, 0, rs1, off));
}

void rv_ret(struct code *c) { rv_jalr(c, RV_ZERO, RV_RA, 0); }

int rv_call_placeholder(struct code *c)
{
    int at = c->len;
    code_u32(c, rv_enc_u(OP_AUIPC, RV_RA, 0));
    code_u32(c, rv_enc_i(OP_JALR, RV_RA, 0, RV_RA, 0));
    return at;
}

/* ---- traps ------------------------------------------------------------ */

/* A CSR instruction's top 12 bits are an UNSIGNED csr number, not the
 * signed immediate every other I-type has there. Same bit positions,
 * different field, so it gets its own packer: pushing 0xc00 through
 * rv_enc_i is a range error, and widening rv_enc_i to accept it would
 * stop it catching a real out-of-range offset. */
static unsigned long enc_csr(int f3, int rd, int rs1, unsigned csr)
{
    if (csr > 0xfff)
        internal_error("riscv: csr 0x%x is not 12 bits", csr);
    return (unsigned long)OP_SYSTEM | ((unsigned long)rd << 7) |
           ((unsigned long)f3 << 12) | ((unsigned long)rs1 << 15) |
           ((unsigned long)csr << 20);
}

/* `unimp`: csrrw x0, cycle, x0 -- a write to a read-only CSR, which the
 * spec requires to trap. It is what the assembler's `unimp` produces and
 * what llvm-objdump prints back. */
void rv_unimp(struct code *c)
{
    code_u32(c, enc_csr(1, RV_ZERO, RV_ZERO, 0xc00));
}

void rv_ebreak(struct code *c)
{
    code_u32(c, enc_csr(0, RV_ZERO, RV_ZERO, 1));
}
