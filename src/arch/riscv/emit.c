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

/* ---- the C extension: compressing what was already encoded ----------
 *
 * Every RISC-V instruction this backend emits passes through rv_w(),
 * so compression happens in ONE place: take the canonical 32-bit
 * encoding, ask whether a 16-bit form denotes exactly the same thing,
 * and emit that instead.
 *
 * That is deliberately not how GCC and LLVM do it. There a compressed
 * instruction is a separate definition the selector may choose, or a
 * later relaxation pass over a stream. Both mean two descriptions of
 * one instruction, and this file's whole argument (see the header) is
 * that a second description is a second chance to be wrong. Deriving
 * the short form FROM the long one cannot disagree with it: if the
 * decode below is wrong, the referee catches it on the instruction
 * itself rather than on some path the selector happens to take.
 *
 * What is deliberately NOT compressed:
 *
 *   branches and jumps -- their displacement is patched in later
 *     (rv_patch_b / rv_patch_j), so the word passing through here
 *     carries a placeholder, and a decision made on a placeholder is
 *     not a decision. c.j and c.beqz are left on the table.
 *   anything inside a no-compress region -- three sequences measure a
 *     distance in bytes rather than recording an offset (the skip in
 *     codegen.c, the auipc/jalr call pair, and the linker's entry
 *     stub sized by rv_li_len). Shortening an instruction under them
 *     moves their target.
 *
 * Returns the 16-bit encoding, or 0 -- which is not a valid compressed
 * instruction (it is a defined illegal encoding), so it doubles as
 * "no short form".
 */

/* x8..x15 are the eight registers the three-bit fields can name. */
static int creg(int r) { return r >= 8 && r <= 15 ? r - 8 : -1; }

static unsigned cr_ca(unsigned op, unsigned f6, int rd_, unsigned f2, int rs2_)
{
    return op | ((unsigned)rs2_ << 2) | (f2 << 5) | ((unsigned)rd_ << 7) |
           (f6 << 10);
}

unsigned rv_compress(unsigned long w, int xlen)
{
    unsigned op    = (unsigned)(w & 0x7f);
    int rd         = (int)((w >> 7) & 0x1f);
    unsigned f3    = (unsigned)((w >> 12) & 7);
    int rs1        = (int)((w >> 15) & 0x1f);
    int rs2        = (int)((w >> 20) & 0x1f);
    unsigned f7    = (unsigned)((w >> 25) & 0x7f);
    /* Both immediates are TWELVE bits and must be sign-extended from
     * bit 11 by hand. `(long)w >> 20` does not do it: w holds a
     * zero-extended 32-bit word, so the arithmetic shift has only
     * zeros above it and -32 arrives as 4064. Every negative immediate
     * then failed the range test and nothing with one was ever
     * compressed -- which is a missed encoding rather than a wrong
     * one, and so invisible to anything but a referee that sweeps the
     * operand space. */
    int immi       = (int)((w >> 20) & 0xfff);                 /* I */
    if (immi & 0x800) immi -= 0x1000;
    int imms       = (int)((((w >> 25) & 0x7f) << 5) | ((w >> 7) & 0x1f));
    if (imms & 0x800) imms -= 0x1000;                          /* S */
    int rdc = creg(rd), rs1c = creg(rs1), rs2c = creg(rs2);

    switch (op) {
    case 0x13:                                                 /* OP_IMM */
        if (f3 == 0) {                                         /* addi */
            if (rd == 0 && rs1 == 0 && immi == 0)
                return 0x0001;                                 /* c.nop */
            if (rd != 0 && rs1 == rd && immi != 0 && immi >= -32 && immi < 32)
                return 0x0001u | ((unsigned)(immi & 0x1f) << 2) |
                       ((unsigned)rd << 7) | ((unsigned)(immi < 0) << 12);
            if (rd != 0 && rs1 == 0 && immi >= -32 && immi < 32)
                return 0x4001u | ((unsigned)(immi & 0x1f) << 2) |
                       ((unsigned)rd << 7) | ((unsigned)(immi < 0) << 12);
            /* c.mv. This backend spells a register move `addi rd, rs, 0`
             * (rv_mv), not `add rd, zero, rs`, so the C2 form has to be
             * reached from the OP_IMM side as well as the OP side --
             * eleven of the referee's thirteen first misses were this
             * one instruction. rs1 != 0 keeps it clear of c.jr, which
             * shares the opcode and differs by having a zero rs2. */
            if (rd != 0 && rs1 != 0 && immi == 0)
                return 0x8002u | ((unsigned)rs1 << 2) | ((unsigned)rd << 7);
            if (rd == 2 && rs1 == 2 && immi != 0 && (immi & 15) == 0 &&
                immi >= -512 && immi < 512) {                  /* c.addi16sp */
                unsigned i = (unsigned)immi;
                return 0x6101u |
                       (((i >> 5) & 1) << 2) | (((i >> 7) & 3) << 3) |
                       (((i >> 6) & 1) << 5) | (((i >> 4) & 1) << 6) |
                       (((i >> 9) & 1) << 12);
            }
            if (rdc >= 0 && rs1 == 2 && immi > 0 && immi < 1024 &&
                (immi & 3) == 0) {                             /* c.addi4spn */
                unsigned i = (unsigned)immi;
                return 0x0000u | ((unsigned)rdc << 2) |
                       (((i >> 3) & 1) << 5) | (((i >> 2) & 1) << 6) |
                       (((i >> 6) & 0xf) << 7) | (((i >> 4) & 3) << 11);
            }
        }
        if (f3 == 7 && rdc >= 0 && rs1 == rd && immi >= -32 && immi < 32)
            return 0x8801u | ((unsigned)(immi & 0x1f) << 2) |
                   ((unsigned)rdc << 7) | ((unsigned)(immi < 0) << 12);   /* c.andi */
        if (f3 == 1 && rd != 0 && rs1 == rd && f7 <= 1) {       /* slli */
            int sh = (int)((w >> 20) & (xlen == 64 ? 0x3f : 0x1f));
            if (sh != 0 && (xlen == 64 || sh < 32))
                return 0x0002u | ((unsigned)(sh & 0x1f) << 2) |
                       ((unsigned)rd << 7) | ((unsigned)((sh >> 5) & 1) << 12);
        }
        if (f3 == 5 && rdc >= 0 && rs1 == rd) {                 /* srli/srai */
            int sh = (int)((w >> 20) & (xlen == 64 ? 0x3f : 0x1f));
            unsigned kind = (f7 & 0x20) ? 1u : 0u;              /* srai */
            if (sh != 0 && (xlen == 64 || sh < 32))
                return 0x8001u | ((unsigned)(sh & 0x1f) << 2) |
                       ((unsigned)rdc << 7) | (kind << 10) |
                       ((unsigned)((sh >> 5) & 1) << 12);
        }
        return 0;

    case 0x1b:                                                  /* OP_IMM32 */
        /* c.addiw exists only at RV64, and unlike c.addi it permits a
         * zero immediate (addiw rd,rd,0 is the canonical sext.w). */
        if (xlen == 64 && f3 == 0 && rd != 0 && rs1 == rd &&
            immi >= -32 && immi < 32)
            return 0x2001u | ((unsigned)(immi & 0x1f) << 2) |
                   ((unsigned)rd << 7) | ((unsigned)(immi < 0) << 12);
        /* `addiw rd, x0, imm` is c.li, not c.addiw: with a source of
         * x0 the 32-bit result's sign extension is the value itself
         * for anything the six-bit field can hold, so the two are the
         * same instruction. sext.w rd, x0 -- addiw rd, x0, 0 -- is the
         * common way in. */
        if (xlen == 64 && f3 == 0 && rd != 0 && rs1 == 0 &&
            immi >= -32 && immi < 32)
            return 0x4001u | ((unsigned)(immi & 0x1f) << 2) |
                   ((unsigned)rd << 7) | ((unsigned)(immi < 0) << 12);
        return 0;

    case 0x37: {                                                /* lui */
        int hi = (int)(w >> 12) & 0xfffff;
        int s = (hi & 0x80000) ? hi - 0x100000 : hi;
        if (rd != 0 && rd != 2 && s != 0 && s >= -32 && s < 32)
            return 0x6001u | ((unsigned)(s & 0x1f) << 2) |
                   ((unsigned)rd << 7) | ((unsigned)(s < 0) << 12);
        return 0;
    }

    case 0x33:                                                  /* OP */
        if (f3 == 0 && f7 == 0) {                               /* add */
            if (rd != 0 && rs1 == 0 && rs2 != 0)
                return 0x8002u | ((unsigned)rs2 << 2) | ((unsigned)rd << 7);
            /* `add rd, rs1, x0` is a move too -- addition is
             * commutative and x0 is the zero, so either operand may be
             * the one that vanishes. Missing this side cost twelve
             * instructions in the sweep. */
            if (rd != 0 && rs2 == 0 && rs1 != 0)
                return 0x8002u | ((unsigned)rs1 << 2) | ((unsigned)rd << 7);
            if (rd != 0 && rs1 == rd && rs2 != 0)
                return 0x9002u | ((unsigned)rs2 << 2) | ((unsigned)rd << 7);
        }
        if (rdc >= 0 && rs2c >= 0 && rs1 == rd) {
            if (f3 == 0 && f7 == 0x20) return cr_ca(0x8001u, 0x23, rdc, 0, rs2c);
            if (f3 == 4 && f7 == 0)    return cr_ca(0x8001u, 0x23, rdc, 1, rs2c);
            if (f3 == 6 && f7 == 0)    return cr_ca(0x8001u, 0x23, rdc, 2, rs2c);
            if (f3 == 7 && f7 == 0)    return cr_ca(0x8001u, 0x23, rdc, 3, rs2c);
        }
        return 0;

    case 0x3b:                                                  /* OP32 */
        if (xlen == 64 && rdc >= 0 && rs2c >= 0 && rs1 == rd && f3 == 0) {
            if (f7 == 0x20) return cr_ca(0x9001u, 0x27, rdc, 0, rs2c);
            if (f7 == 0)    return cr_ca(0x9001u, 0x27, rdc, 1, rs2c);
        }
        return 0;

    case 0x03:                                                  /* LOAD */
        if (f3 == 2) {                                          /* lw */
            if (rd != 0 && rs1 == 2 && immi >= 0 && immi < 256 &&
                (immi & 3) == 0) {
                unsigned i = (unsigned)immi;
                return 0x4002u | (((i >> 6) & 3) << 2) | (((i >> 2) & 7) << 4) |
                       (((i >> 5) & 1) << 12) | ((unsigned)rd << 7);
            }
            if (rdc >= 0 && rs1c >= 0 && immi >= 0 && immi < 128 &&
                (immi & 3) == 0) {
                unsigned i = (unsigned)immi;
                return 0x4000u | ((unsigned)rdc << 2) | (((i >> 6) & 1) << 5) |
                       (((i >> 2) & 1) << 6) | ((unsigned)rs1c << 7) |
                       (((i >> 3) & 7) << 10);
            }
        }
        if (f3 == 3 && xlen == 64) {                            /* ld */
            if (rd != 0 && rs1 == 2 && immi >= 0 && immi < 512 &&
                (immi & 7) == 0) {
                unsigned i = (unsigned)immi;
                return 0x6002u | (((i >> 6) & 7) << 2) | (((i >> 3) & 3) << 5) |
                       (((i >> 5) & 1) << 12) | ((unsigned)rd << 7);
            }
            if (rdc >= 0 && rs1c >= 0 && immi >= 0 && immi < 256 &&
                (immi & 7) == 0) {
                unsigned i = (unsigned)immi;
                return 0x6000u | ((unsigned)rdc << 2) | (((i >> 6) & 3) << 5) |
                       ((unsigned)rs1c << 7) | (((i >> 3) & 7) << 10);
            }
        }
        return 0;

    case 0x23:                                                  /* STORE */
        if (f3 == 2) {                                          /* sw */
            if (rs1 == 2 && imms >= 0 && imms < 256 && (imms & 3) == 0) {
                unsigned i = (unsigned)imms;
                return 0xc002u | ((unsigned)rs2 << 2) | (((i >> 6) & 3) << 7) |
                       (((i >> 2) & 0xf) << 9);
            }
            if (rs1c >= 0 && rs2c >= 0 && imms >= 0 && imms < 128 &&
                (imms & 3) == 0) {
                unsigned i = (unsigned)imms;
                return 0xc000u | ((unsigned)rs2c << 2) | (((i >> 6) & 1) << 5) |
                       (((i >> 2) & 1) << 6) | ((unsigned)rs1c << 7) |
                       (((i >> 3) & 7) << 10);
            }
        }
        if (f3 == 3 && xlen == 64) {                            /* sd */
            if (rs1 == 2 && imms >= 0 && imms < 512 && (imms & 7) == 0) {
                unsigned i = (unsigned)imms;
                return 0xe002u | ((unsigned)rs2 << 2) | (((i >> 6) & 7) << 7) |
                       (((i >> 3) & 7) << 10);
            }
            if (rs1c >= 0 && rs2c >= 0 && imms >= 0 && imms < 256 &&
                (imms & 7) == 0) {
                unsigned i = (unsigned)imms;
                return 0xe000u | ((unsigned)rs2c << 2) | (((i >> 6) & 3) << 5) |
                       ((unsigned)rs1c << 7) | (((i >> 3) & 7) << 10);
            }
        }
        return 0;

    case 0x67:                                                  /* JALR */
        /* c.jr / c.jalr, only with a zero displacement. `ret` is
         * jalr x0, 0(ra) and becomes c.jr ra, which is two bytes off
         * every function in the program. */
        if (f3 == 0 && immi == 0 && rs1 != 0) {
            if (rd == 0) return 0x8002u | ((unsigned)rs1 << 7);
            if (rd == 1) return 0x9002u | ((unsigned)rs1 << 7);
        }
        return 0;

    case 0x73:                                                  /* SYSTEM */
        /* c.ebreak. `unimp` is deliberately NOT compressed: the 32-bit
         * form this backend emits is a real trapping instruction,
         * where the assembler's `unimp` pseudo becomes the two-byte
         * all-zero ILLEGAL encoding. Both trap; they are not the same
         * instruction, and this file's job is to preserve the one that
         * was encoded. */
        if (w == 0x00100073UL)
            return 0x9002u;
        return 0;

    default:
        return 0;
    }
}

/* Compression is ON for a target that has the C extension and OFF
 * inside the three sequences that measure a distance in bytes rather
 * than recording an offset:
 *
 *   codegen.c's `rv_patch_b(F->t, at, at + 8)`, which skips a jump
 *   codegen.c's auipc/jalr call pair, patched at `at + 4`
 *   link.c's entry stub, sized by rv_li_len()
 *
 * A shorter instruction under any of them moves its target. The flag
 * is a global because compilation is single-threaded and the
 * alternative -- threading it through every encoder -- would put it in
 * forty signatures to be read by three callers.
 */
static int g_rvc = 0;        /* compress? */
static int g_rvc_xlen = 64;  /* ...and for which width */

/* The width comes WITH the switch rather than from target_xlen():
 * this file has no current-width global by design (see the header),
 * and embld links it without the target layer at all. */
void rv_set_compress(int on, int xlen)
{
    g_rvc = on;
    g_rvc_xlen = xlen;
}
int rv_compress_enabled(void) { return g_rvc; }

/* c.unimp -- the all-zero compressed encoding, which the ISA defines as
 * illegal so it traps. Two bytes, for padding a function to alignment
 * when a compressed instruction has left an odd halfword. Emitted
 * directly: it IS the short form, so it must not be handed to the
 * compressor. */
void rv_cunimp(struct code *c) { code_u16(c, 0x0000); }

/* The one place a RISC-V instruction becomes bytes. */
void rv_w(struct code *c, unsigned long w)
{
    if (g_rvc) {
        unsigned s = rv_compress(w, g_rvc_xlen);
        if (s) {
            code_u16(c, s);
            return;
        }
    }
    code_u32(c, w);
}

void rv_mv(struct code *c, int rd, int rs)
{
    rv_w(c, rv_enc_i(OP_IMM, rd, 0, rs, 0));
}

void rv_lui(struct code *c, int rd, long hi20)
{
    rv_w(c, rv_enc_u(OP_LUI, rd, hi20));
}

void rv_auipc(struct code *c, int rd, long hi20)
{
    rv_w(c, rv_enc_u(OP_AUIPC, rd, hi20));
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
        if (emit) rv_w(c, rv_enc_i(OP_IMM, rd, 0, RV_ZERO, (int)v));
        return 4;
    }
    if (rv_fits(v, 32)) {
        int lo = lo12_of(v);
        int n = 4;
        if (emit) rv_w(c, rv_enc_u(OP_LUI, rd, hi20_of(v)));
        if (lo) {
            /* ADDI or ADDIW, and the choice is not cosmetic.
             *
             * `lui` SIGN-EXTENDS bit 31. When the high half's top bit is
             * set the register holds a negative 64-bit value, and a plain
             * `addi` leaves it negative -- lui(0x80000)+addi(-1) for
             * 0x7fffffff gives 0xffffffff7fffffff. `addiw` re-narrows the
             * sum to 32 bits and sign-extends it, which is the answer.
             *
             * When that bit is CLEAR no sign extension happened and
             * `addi` is exact. Both are correct there, and `addi` is what
             * every RISC-V assembler emits -- which matters because
             * tests/golden/riscv-asm.sh compares these bytes against
             * llvm-mc's, and a difference that is only a preference is
             * one somebody has to re-explain every time they read it.
             *
             * addiw does not exist at RV32, so there the question does
             * not arise. */
            int narrow = xlen == 64 && (hi20_of(v) & 0x80000) != 0;
            if (emit)
                rv_w(c, rv_enc_i(narrow ? OP_IMM32 : OP_IMM,
                                     rd, 0, rd, lo));
            n += 4;
        }
        return n;
    }
    int lo = lo12_of(v);
    long long hi = (v - lo) >> 12;
    int n = li_emit(c, rd, hi, xlen, emit);
    if (emit) rv_w(c, rv_enc_r(OP_IMM, rd, 1, rd, 12, 0)); /* slli rd,rd,12 */
    n += 4;
    if (lo) {
        if (emit) rv_w(c, rv_enc_i(OP_IMM, rd, 0, rd, lo));
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
    rv_w(c, rv_enc_r(w ? OP_OP32 : OP_OP, rd, alu_enc[op].f3, rs1, rs2,
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
    rv_w(c, rv_enc_i(w ? OP_IMM32 : OP_IMM, rd, alu_enc[op].f3, rs1, imm));
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
    rv_w(c, rv_enc_r(w ? OP_IMM32 : OP_IMM, rd, alu_enc[op].f3, rs1,
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
    rv_w(c, rv_enc_r(w ? OP_OP32 : OP_OP, rd, muldiv_f3[op], rs1, rs2, 1));
}

/* ---- the A extension ------------------------------------------------- */

void rv_amo(struct code *c, enum rv_amo op, int rd, int rs1, int rs2,
            int ord, int w)
{
    if (ord < 0 || ord > 3)
        internal_error("riscv: amo ordering %d is not one of the four", ord);
    if (op == RV_LR && rs2 != RV_ZERO)
        internal_error("riscv: lr takes no second source register");
    /* funct5 at the top of the seven-bit field, then aq (bit 26) and rl
     * (bit 25) beneath it -- which is what makes this an ordinary R-type as
     * far as the packer is concerned. */
    rv_w(c, rv_enc_r(0x2f, rd, w ? 3 : 2, rs1, rs2,
                     ((int)op << 2) | ord));
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
    rv_w(c, rv_enc_i(OP_LOAD, rd, f3, rs1, off));
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
    rv_w(c, rv_enc_s(OP_STORE, f3, rs1, rs2, off));
}

/* ---- control flow ----------------------------------------------------- */

int rv_b_placeholder(struct code *c, int cond, int rs1, int rs2)
{
    /* No compression here: a branch whose displacement is patched later. A decision made on a
     * placeholder is not a decision, and the patch sites
     * address these by a fixed distance. */
    int save_rvc = g_rvc;
    g_rvc = 0;

    int at = c->len;
    rv_w(c, rv_enc_b(OP_BRANCH, cond, rs1, rs2, 0));
    g_rvc = save_rvc;
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
    /* No compression here: a jump whose displacement is patched later. A decision made on a
     * placeholder is not a decision, and the patch sites
     * address these by a fixed distance. */
    int save_rvc = g_rvc;
    g_rvc = 0;

    int at = c->len;
    rv_w(c, rv_enc_j(OP_JAL, rd, 0));
    g_rvc = save_rvc;
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
    rv_w(c, rv_enc_i(OP_JALR, rd, 0, rs1, off));
}

void rv_ret(struct code *c) { rv_jalr(c, RV_ZERO, RV_RA, 0); }

/* auipc rd, 0 ; addi rd, rd, 0 -- the two halves of a PC-relative
 * address, both immediates left for a relocation. Returns the offset of
 * the auipc; the addi is four bytes after it.
 *
 * Never compressed, and that is the whole reason this is a function
 * rather than two calls at each site: `addi rd, rd, 0` IS c.mv, so the
 * placeholder compressed itself into a register move and the
 * relocation then wrote its low half over the next instruction. Three
 * sites emitted this pair by hand and all three broke the moment the C
 * extension was switched on.
 */
int rv_pcrel_pair(struct code *c, int rd)
{
    int save_rvc = g_rvc;
    int at = c->len;
    g_rvc = 0;
    rv_auipc(c, rd, 0);
    rv_alu_imm(c, RV_ADD, rd, rd, 0, 0);
    g_rvc = save_rvc;
    return at;
}

int rv_call_placeholder(struct code *c)
{
    /* No compression here: the auipc/jalr pair, patched at at+0 and at+4. A decision made on a
     * placeholder is not a decision, and the patch sites
     * address these by a fixed distance. */
    int save_rvc = g_rvc;
    g_rvc = 0;

    int at = c->len;
    rv_w(c, rv_enc_u(OP_AUIPC, RV_RA, 0));
    rv_w(c, rv_enc_i(OP_JALR, RV_RA, 0, RV_RA, 0));
    g_rvc = save_rvc;
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
    rv_w(c, enc_csr(1, RV_ZERO, RV_ZERO, 0xc00));
}

void rv_ebreak(struct code *c)
{
    rv_w(c, enc_csr(0, RV_ZERO, RV_ZERO, 1));
}
