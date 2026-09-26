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

/* ---- memory ---------------------------------------------------------- */

/* rd = *(rs1 + off), sized and signed. `size` is 1/2/4/8 and `sign` says
 * whether a narrow load sign-extends; an 8-byte access is RV64 only. The
 * offset is a 12-bit signed field, and a frame deeper than 2047 bytes goes
 * through a scratch in the codegen, not here. */
void rv_load(struct code *c, int rd, int rs1, int off, int size, int sign);
void rv_store(struct code *c, int rs2, int rs1, int off, int size);

/* ---- control flow ----------------------------------------------------- */

enum { RV_BEQ, RV_BNE, RV_BLT = 4, RV_BGE = 5, RV_BLTU = 6, RV_BGEU = 7 };

/* A branch to a target not yet known: emitted with a zero displacement and
 * patched later. Returns the offset of the instruction, for rv_patch_b(). */
int rv_b_placeholder(struct code *c, int cond, int rs1, int rs2);
void rv_patch_b(struct code *c, int at, int target);

int rv_j_placeholder(struct code *c, int rd);   /* jal rd, . */
void rv_patch_j(struct code *c, int at, int target);

void rv_jalr(struct code *c, int rd, int rs1, int off);
void rv_ret(struct code *c);                    /* jalr zero, 0(ra) */

/* A call to a symbol the linker will resolve: `auipc ra, 0` + `jalr ra`,
 * the pair that ONE R_RISCV_CALL relocation patches. Returns the offset of
 * the auipc, which is where the relocation goes — the jalr is patched by
 * the same relocation and carries no site of its own. */
int rv_call_placeholder(struct code *c);

/* ---- traps ------------------------------------------------------------ */

/* The guaranteed-illegal instruction, 0xc0001073. This is what IR_UD2
 * lowers to. A load from address zero is NOT a substitute: on a board with
 * memory or a trap handler there it simply succeeds, which is how an
 * unreachable path became a silent fallthrough on Cortex-M. */
void rv_unimp(struct code *c);
void rv_ebreak(struct code *c);

#endif
