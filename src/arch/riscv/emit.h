/* RISC-V instruction encoding, RV32IM and RV64IM (D-016).
 *
 * One file for both widths. Where an instruction exists only at RV64 the
 * encoder takes it on trust that the caller checked `target_xlen()`; where
 * the two widths differ in the OPERATION rather than in what exists, the
 * function takes an explicit `w` (word) flag and the caller has to have
 * decided. There is no "current width" global in here.
 *
 * ---- why this file is a third the size of thumb/emit.c -----------------
 *
 * RISC-V has six instruction formats and every instruction is four bytes.
 * So this file does NOT have one function per mnemonic that knows its own
 * bit layout: it has six `enc_*` packers and a set of thin wrappers that
 * pass (opcode, funct3, funct7). A new instruction is three constants and
 * a line, and it cannot get a field in the wrong place, because it does
 * not name the places. That is the difference between deriving an encoding
 * and restating it, and the Thumb encoder's three bugs were all restating.
 *
 * The immediate fields are the part worth reading twice. Every one of them
 * is SIGN-EXTENDED, and two of them are split across non-adjacent bits (S
 * and B) with a third scrambled outright (J). They are packed here, once,
 * from an ordinary `int`, and rv_check() refuses a value that does not fit
 * rather than truncating it into a jump somewhere else.
 *
 * tools/riscvcheck round-trips every encoder below through llvm-objdump.
 */
#ifndef EMBCC_ARCH_RISCV_EMIT_H
#define EMBCC_ARCH_RISCV_EMIT_H

#include "../code.h"

/* The register file, by ABI name (psABI table 18.2). x0 reads as zero and
 * discards writes, which is what makes RISC-V need so few real
 * instructions: `mv` is `addi rd, rs, 0`, `not` is `xori rd, rs, -1`, a
 * bare `j` is `jal x0`, and `ret` is `jalr x0, 0(ra)`. */
enum {
    RV_ZERO = 0,  /* hardwired 0 */
    RV_RA   = 1,  /* return address */
    RV_SP   = 2,
    RV_GP   = 3, RV_TP = 4,
    RV_T0   = 5, RV_T1 = 6, RV_T2 = 7,
    RV_FP   = 8,  /* s0 */
    RV_S1   = 9,
    RV_A0   = 10, RV_A1 = 11, RV_A2 = 12, RV_A3 = 13,
    RV_A4   = 14, RV_A5 = 15, RV_A6 = 16, RV_A7 = 17,
    RV_S2   = 18,
    RV_T3   = 28, RV_T4 = 29, RV_T5 = 30, RV_T6 = 31
};

/* Scratch. Three of them, where ARMv7-M had to borrow one from the
 * callee-saved file: t0-t2 are caller-saved and are not argument
 * registers, so nothing in the prologue pays for them. */
enum { RV_ACC = RV_T0, RV_TMP = RV_T1, RV_TMP2 = RV_T2 };

/* The argument registers, in order, so a caller can index them. */
#define RV_NARGREG 8
extern const int rv_argreg[RV_NARGREG];

/* ---- the six formats ------------------------------------------------ */
/* Exposed so an instruction this header has not wrapped can still be
 * emitted correctly rather than hand-assembled by its caller. */
unsigned long rv_enc_r(int op, int rd, int f3, int rs1, int rs2, int f7);
unsigned long rv_enc_i(int op, int rd, int f3, int rs1, int imm);
unsigned long rv_enc_s(int op, int f3, int rs1, int rs2, int imm);
unsigned long rv_enc_b(int op, int f3, int rs1, int rs2, int imm);
unsigned long rv_enc_u(int op, int rd, long imm);   /* imm is the HI20 field */
unsigned long rv_enc_j(int op, int rd, int imm);

/* Does a signed value fit in `bits`?  Callers that can fall back to a
 * longer sequence ask first; callers that cannot let the encoder abort. */
int rv_fits(long long v, int bits);

/* ---- moves and constants -------------------------------------------- */

void rv_mv(struct code *c, int rd, int rs);            /* addi rd, rs, 0 */
void rv_lui(struct code *c, int rd, long hi20);
void rv_auipc(struct code *c, int rd, long hi20);

/* rd = v, in as few instructions as the value needs: one `addi` for a
 * 12-bit signed value, `lui`+`addi` for a 32-bit one, and a shift-and-add
 * chain beyond that. `xlen` is 32 or 64; at 32 a value that does not fit
 * is a bug in the caller, not a longer sequence. */
void rv_li(struct code *c, int rd, long long v, int xlen);

/* How many bytes rv_li would emit, without emitting them — the codegen
 * needs the size before it commits to a branch distance. */
int rv_li_len(long long v, int xlen);

/* The C extension, derived rather than restated: given a canonical
 * 32-bit encoding, the 16-bit form that denotes exactly the same
 * instruction, or 0 when there is none (0 is a defined ILLEGAL
 * compressed encoding, so it cannot be mistaken for an answer).
 *
 * Compression happens at one chokepoint rather than in the selector,
 * so it cannot disagree with the long form it came from -- see the
 * note above the implementation. tools/riscvcheck --c32/--c64 checks
 * every form against llvm-mc, which compresses on its own. */
unsigned rv_compress(unsigned long w, int xlen);

/* Every RISC-V instruction becomes bytes here, so this is where the
 * compressed form is substituted. rv_set_compress() turns it on for a
 * target that has the C extension, and OFF around the three sequences
 * that measure a distance in bytes instead of recording an offset --
 * see the note above rv_w's implementation. */
void rv_w(struct code *c, unsigned long w);
void rv_cunimp(struct code *c);   /* two bytes, and it traps */

/* auipc rd,0 ; addi rd,rd,0 -- a PC-relative address's two halves,
 * both left for relocation, and never compressed (addi rd,rd,0 is
 * c.mv). Returns the auipc's offset; the addi is at +4. */
int rv_pcrel_pair(struct code *c, int rd);
/* ...and one whose target is in this same buffer, patched here once its
 * place is known: `auipc rd, %pcrel_hi; addi rd, rd, %pcrel_lo` with no
 * relocation (a computed goto's &&label). */
void rv_patch_pcrel_pair(struct code *c, int at, int target);
void rv_set_compress(int on, int xlen);
int  rv_compress_enabled(void);

/* ---- arithmetic and logic ------------------------------------------- */

/* The operations, numbered by their funct3 where that is unique and by a
 * private number where it is not (sub and sra share funct3 with add and
 * srl, and differ in funct7). One enum drives the register form, the
 * immediate form and the RV64 word forms. */
enum {
    RV_ADD, RV_SUB, RV_SLL, RV_SLT, RV_SLTU,
    RV_XOR, RV_SRL, RV_SRA, RV_OR, RV_AND
};

/* rd = rs1 <op> rs2. `w` selects the RV64 32-bit form (addw/subw/sllw/
 * srlw/sraw); it is only defined for those five, and only at RV64. */
void rv_alu(struct code *c, int op, int rd, int rs1, int rs2, int w);

/* rd = rs1 <op> imm, for the ops that have an I-type immediate form. sub
 * has none (add a negative) and neither do the shifts, whose immediate is
 * an AMOUNT and not a signed field -- rv_shift_imm takes those, and takes
 * xlen because how wide the amount may be depends on it. Both refusals
 * are checked, not documented. */
void rv_alu_imm(struct code *c, int op, int rd, int rs1, int imm, int w);
void rv_shift_imm(struct code *c, int op, int rd, int rs1, int amt,
                  int w, int xlen);

/* The M extension. `op` is one of these; `w` selects mulw/divw/divuw/
 * remw/remuw at RV64. mulh/mulhu/mulhsu have no word form. */
enum { RV_MUL, RV_MULH, RV_MULHSU, RV_MULHU, RV_DIV, RV_DIVU, RV_REM, RV_REMU };
void rv_muldiv(struct code *c, int op, int rd, int rs1, int rs2, int w);

/* ---- the A extension: atomics ----------------------------------------
 *
 * Hazard3 (the RP2350's RISC-V core, and the RTOS requirements' fourth
 * target) is RV32IMAC, so these are part of the baseline rather than an
 * option -- an RTOS needs a lock.
 *
 * All eleven share one encoding: opcode 0x2f, funct3 selecting the width,
 * and a seven-bit field holding funct5 at its top with the two ordering
 * bits below it -- `aq` (acquire) at bit 26 and `rl` (release) at 25. That
 * is the same seven-bit slot an R-type's funct7 occupies, which is why
 * rv_enc_r packs these too and there is no second packer to keep in step.
 *
 * The funct5 values were read off `llvm-mc -show-encoding`, not a table. */
enum rv_amo {
    RV_AMOADD  = 0x00, RV_AMOSWAP = 0x01, RV_LR      = 0x02, RV_SC   = 0x03,
    RV_AMOXOR  = 0x04, RV_AMOOR   = 0x08, RV_AMOAND  = 0x0C,
    RV_AMOMIN  = 0x10, RV_AMOMAX  = 0x14, RV_AMOMINU = 0x18,
    RV_AMOMAXU = 0x1C
};
/* The ordering suffix. AQRL is what a C11 seq_cst operation needs, and is
 * what every lowering here uses: an RTOS lock that is merely relaxed is a
 * lock that does not work on an out-of-order core. */
enum { RV_ORD_RELAXED = 0, RV_ORD_RL = 1, RV_ORD_AQ = 2, RV_ORD_AQRL = 3 };
/* `w` is 0 for .w (four bytes) and 1 for .d (eight, RV64 only). `lr` has no
 * rs2 and takes RV_ZERO there. */
/* `fence pred, succ`: each a subset of i/o/r/w as bits 3..0
 * (RV_FENCE_I .. RV_FENCE_W). */
enum { RV_FENCE_I = 8, RV_FENCE_O = 4, RV_FENCE_R = 2, RV_FENCE_W = 1 };
void rv_fence(struct code *c, unsigned pred, unsigned succ);
void rv_amo(struct code *c, enum rv_amo op, int rd, int rs1, int rs2,
            int ord, int w);

/* ---- memory ---------------------------------------------------------- */

/* rd = *(rs1 + off), sized and signed. `size` is 1/2/4/8 and `sign` says
 * whether a narrow load sign-extends. These take xlen because the two
 * widths differ in what EXISTS here and not just in what it means: `ld`,
 * `sd` and `lwu` are RV64-only, and at RV32 a four-byte load is the whole
 * register and has no unsigned form. The offset is a 12-bit signed field;
 * a frame deeper than 2047 bytes goes through a scratch in the codegen,
 * not here. */
void rv_load(struct code *c, int rd, int rs1, int off, int size, int sign,
             int xlen);
void rv_store(struct code *c, int rs2, int rs1, int off, int size, int xlen);

/* ---- control flow ----------------------------------------------------- */

enum { RV_BEQ, RV_BNE, RV_BLT = 4, RV_BGE = 5, RV_BLTU = 6, RV_BGEU = 7 };

/* A branch to a target not yet known: emitted with a zero displacement and
 * patched later. Returns the offset of the instruction, for rv_patch_b(). */
int rv_b_placeholder(struct code *c, int cond, int rs1, int rs2);
void rv_patch_b(struct code *c, int at, int target);

int rv_j_placeholder(struct code *c, int rd);   /* jal rd, . */
void rv_patch_j(struct code *c, int at, int target);
int rv_c_placeholder(struct code *c);
int rv_patch_cj(struct code *c, int at, int target);
int rv_patch_cb(struct code *c, int at, int ne, int rs1, int target);
int rv_patch_b_checked(struct code *c, int at, int target);

void rv_jalr(struct code *c, int rd, int rs1, int off);
void rv_ret(struct code *c);                    /* jalr zero, 0(ra) */
/* An interrupt handler's return: mret (machine mode) or, with
 * `supervisor`, sret -- SYSTEM instructions whose funct12 is 0x302 and
 * 0x102, the same packer as every I-type, and never compressed. */
void rv_xret(struct code *c, int supervisor);

/* A call to a symbol the linker will resolve: `auipc ra, 0` + `jalr ra`,
 * the pair that ONE R_RISCV_CALL relocation patches. Returns the offset of
 * the auipc, which is where the relocation goes — the jalr is patched by
 * the same relocation and carries no site of its own. */
int rv_call_placeholder(struct code *c);
int rv_tail_placeholder(struct code *c);

/* ---- the F and D extensions ----------------------------------------------
 *
 * Thirty-two floating-point registers f0-f31, numbered 0-31 here as the
 * integer ones are; each is FLEN wide (32 with F alone, 64 with D) and
 * holds a float or a double. A float in a 64-bit register is NaN-BOXED --
 * its upper half all ones -- which flw and fmv.w.x do and every
 * single-precision instruction checks, so a float only ever enters one by
 * those two.
 *
 * One encoding shape for nearly everything: OP-FP (0x53), an R-type whose
 * funct7 is funct5 << 2 | fmt, fmt 0 for single and 1 for double, with the
 * rounding mode in funct3 where the operation rounds. `dbl` picks fmt
 * throughout. The rounding mode is DYN (the fcsr's, round-to-nearest-even
 * after reset) for arithmetic, RTZ for a conversion to an integer -- C's
 * truncation -- and RNE for the conversions that are always exact
 * (fcvt.d.s, fcvt.d.w), which is what llvm-mc writes for those. */
enum {
    RV_FT0 = 0, RV_FT1 = 1, RV_FT2 = 2, RV_FT3 = 3,
    RV_FS0 = 8, RV_FS1 = 9,
    RV_FA0 = 10,                 /* fa0-fa7: f10-f17, the argument registers */
    RV_FS2 = 18,                 /* fs2-fs11: f18-f27 */
    RV_FT8 = 28                  /* ft8-ft11: f28-f31 */
};
void rv_fload(struct code *c, int frd, int rs1, int off, int dbl);    /* flw/fld */
void rv_fstore(struct code *c, int frs2, int rs1, int off, int dbl);  /* fsw/fsd */
/* fadd, fsub, fmul, fdiv: the funct5 values */
enum { RV_FADD = 0x00, RV_FSUB = 0x01, RV_FMUL = 0x02, RV_FDIV = 0x03 };
void rv_farith(struct code *c, int op, int frd, int frs1, int frs2, int dbl);
void rv_fsqrt(struct code *c, int frd, int frs1, int dbl);
/* sign injection: fsgnj frd, frs, frs is fmv; fsgnjn is fneg, fsgnjx fabs */
enum { RV_FSGNJ = 0, RV_FSGNJN = 1, RV_FSGNJX = 2 };
void rv_fsgnj(struct code *c, int kind, int frd, int frs1, int frs2, int dbl);
void rv_fmv(struct code *c, int frd, int frs, int dbl);              /* fmv.s/.d */
/* rd = (frs1 OP frs2), 0 or 1, in an INTEGER register. Each is false for
 * an unordered pair, which is C's answer for every relation but != */
enum { RV_FLE = 0, RV_FLT = 1, RV_FEQ = 2 };
void rv_fcmp(struct code *c, int kind, int rd, int frs1, int frs2, int dbl);
/* The integer side of a conversion: 0 a signed word, 1 an unsigned one,
 * 2 a signed doubleword and 3 an unsigned one (RV64 only). */
enum { RV_CVT_W = 0, RV_CVT_WU = 1, RV_CVT_L = 2, RV_CVT_LU = 3 };
void rv_fcvt_to_int(struct code *c, int rd, int frs1, int ity, int dbl);
void rv_fcvt_from_int(struct code *c, int frd, int rs1, int ity, int dbl);
/* fcvt.d.s (to_dbl) or fcvt.s.d */
void rv_fcvt_fp(struct code *c, int frd, int frs1, int to_dbl);
/* fmv.x.w / fmv.x.d: the bits into an integer register (fmv.x.w
 * sign-extends at RV64, which is the 32-bit value's invariant there);
 * fmv.w.x / fmv.d.x the other way. The .d forms are RV64 only. */
void rv_fmv_to_x(struct code *c, int rd, int frs1, int dbl);
void rv_fmv_from_x(struct code *c, int frd, int rs1, int dbl);

/* ---- traps ------------------------------------------------------------ */

/* The guaranteed-illegal instruction, 0xc0001073. This is what IR_UD2
 * lowers to. A load from address zero is NOT a substitute: on a board with
 * memory or a trap handler there it simply succeeds, which is how an
 * unreachable path became a silent fallthrough on Cortex-M. */
void rv_unimp(struct code *c);
void rv_ebreak(struct code *c);

#endif
