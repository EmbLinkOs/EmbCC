/* Thumb-2 instruction encoding for ARMv7-M (D-015).
 *
 * Exactly the encodings the Thumb codegen emits; an instruction this file
 * cannot encode is a missing function, which fails at build time — never a
 * silently wrong halfword. tools/thumbcheck round-trips every encoder here
 * through llvm-objdump, which is the only defence a backend that assembles
 * its own instructions has.
 *
 * ---- the thing this instruction set does that the other two do not ----
 *
 * A Thumb 16-bit data-processing instruction ALWAYS sets the flags. There
 * is no two-byte `and` — only `ands`. So on this target "make it smaller"
 * and "is NZCV dead here?" are the same question, which is why every ALU
 * encoder below takes an explicit `s`: the caller has to have decided.
 * Passing s=0 is always safe and always four bytes; passing s=1 when a
 * later branch reads the flags is a miscompile, so the codegen only does
 * it where it can see the flags die.
 *
 * Widths: ARMv7-M registers are 32 bits and so is every operation here.
 * A `size` of 1/2/4 with a `sign` selects among the loads and stores; an
 * eight-byte value is carried as a register pair by the codegen, never by
 * this file.
 *
 * Frame slots are addressed [sp, #off] with a non-negative off — the
 * unsigned-offset forms reach 4095 bytes from sp, and anything past that
 * goes through a scratch (see t_ldst_imm).
 */
#ifndef EMBCC_ARCH_THUMB_EMIT_H
#define EMBCC_ARCH_THUMB_EMIT_H

#include "../code.h"

/* Register roles. AAPCS32: r0-r3 argument and caller-saved, r4-r11
 * callee-saved, r12 (IP) caller-saved and the ABI's own scratch, r13 sp,
 * r14 lr, r15 pc.
 *
 * Two scratch registers, not aarch64's four: ARM32 has exactly one
 * register that is neither an argument nor callee-saved (r12), so the
 * second is taken from the callee-saved file and paid for in the prologue
 * — which is why the prologue's register mask is PATCHED after the body
 * is emitted rather than decided before it. */
enum {
    T_R0 = 0, T_R1 = 1, T_R2 = 2, T_R3 = 3,
    T_TMP = 11,   /* second scratch: callee-saved, saved only when used */
    T_ACC = 12,   /* first scratch: IP, the ABI's own, never needs saving */
    T_SP  = 13,
    T_LR  = 14,
    T_PC  = 15
};

/* Condition codes, in the architectural order the encodings use. */
enum {
    T_EQ = 0, T_NE = 1, T_CS = 2, T_CC = 3, T_MI = 4, T_PL = 5,
    T_VS = 6, T_VC = 7, T_HI = 8, T_LS = 9, T_GE = 10, T_LT = 11,
    T_GT = 12, T_LE = 13, T_AL = 14
};

/* The data-processing operations, numbered as the 32-bit encoding's op
 * field has them so one table drives both widths. */
enum {
    T_OP_AND = 0, T_OP_BIC = 1, T_OP_ORR = 2, T_OP_ORN = 3, T_OP_EOR = 4,
    T_OP_ADD = 8, T_OP_ADC = 10, T_OP_SBC = 11, T_OP_SUB = 13, T_OP_RSB = 14
};

/* The shifts, numbered as the `type` field has them. */
enum { T_SH_LSL = 0, T_SH_LSR = 1, T_SH_ASR = 2, T_SH_ROR = 3 };

/* ---- moves ---------------------------------------------------------- */

/* rd = rm. Two bytes for any pair of registers (the T1 form reaches the
 * high ones through its H bits) and it never touches the flags. */
void t_mov_reg(struct code *c, int rd, int rm);

/* rd = imm, whatever the constant. One halfword-pair `movw` below 65536,
 * `movw`+`movt` above, and the 16-bit `movs` when the value fits eight
 * bits and the caller says the flags are dead. */
void t_mov_imm(struct code *c, int rd, long imm, int s);

/* rd = ~rm. */
void t_mvn_reg(struct code *c, int rd, int rm, int s);

/* ---- data processing ------------------------------------------------ */

/* rd = rn <op> rm. */
void t_alu_reg(struct code *c, int op, int rd, int rn, int rm, int s);

/* rd = rn <op> imm, for an immediate t_imm_ok() accepts. The caller
 * checks first; this refuses (writes nothing and returns 0) rather than
 * encode a value the field cannot hold. */
int t_alu_imm(struct code *c, int op, int rd, int rn, long imm, int s);

/* Can a 32-bit data-processing instruction carry this immediate? The
 * ARMv7-M "modified immediate" is an 8-bit value either rotated to any
 * of 24 positions or replicated across the word in one of three fixed
 * patterns, so most small constants fit and most large ones do not. */
int t_imm_ok(long imm);

/* rd = rn + imm / rn - imm with the 12-bit UNROTATED field (`addw`,
 * `subw`), which reaches any 0..4095 and is what stack offsets use.
 * Never sets flags — the wide-immediate forms have no S bit. */
void t_addw(struct code *c, int rd, int rn, long imm);
void t_subw(struct code *c, int rd, int rn, long imm);

/* rd = rm <shift> #sh, with sh in 0..31 (0 with LSL is a plain move). */
void t_shift_imm(struct code *c, int op, int rd, int rm, int sh, int s);
/* rd = rn <shift> rm, by the low byte of rm. */
void t_shift_reg(struct code *c, int op, int rd, int rn, int rm, int s);

void t_mul(struct code *c, int rd, int rn, int rm);
void t_mla(struct code *c, int rd, int rn, int rm, int ra);
void t_mls(struct code *c, int rd, int rn, int rm, int ra);
/* ARMv7-M has hardware divide; ARMv6-M does not, which is one reason this
 * target is v7-M and not "Cortex-M". */
void t_div(struct code *c, int rd, int rn, int rm, int sign);
/* rdlo:rdhi = rn * rm, the 64-bit product. */
void t_mull(struct code *c, int rdlo, int rdhi, int rn, int rm, int sign);

void t_cmp_reg(struct code *c, int rn, int rm);
void t_cmp_imm(struct code *c, int rn, long imm);   /* t_imm_ok(imm) */
void t_tst_reg(struct code *c, int rn, int rm);

/* rd = extend(rm), size 1 or 2, `sign` for the signed forms. */
void t_ext(struct code *c, int rd, int rm, int size, int sign);
void t_clz(struct code *c, int rd, int rm);
/* rd = rm with its bytes reversed (byte order, not bit order). */
void t_rev(struct code *c, int rd, int rm);

/* ---- memory --------------------------------------------------------- */

/* rt = [rn + off] / [rn + off] = rt. `off` may be any value: the encoder
 * picks the scaled unsigned form, the signed 8-bit form or the 12-bit
 * form, and refuses (returns 0, writes nothing) when none reaches — the
 * caller must then materialise the address itself. */
int t_ldst_imm(struct code *c, int rt, int rn, long off, int size, int sign,
               int store);

/* rt = [rn + (rm << shift)], shift in 0..3. */
void t_ldst_reg(struct code *c, int rt, int rn, int rm, int shift, int size,
                int sign, int store);

/* rd = sp + off, for taking a frame slot's address. */
void t_add_sp(struct code *c, int rd, long off);
/* sp += imm / sp -= imm, imm a multiple of four. */
void t_sp_adjust(struct code *c, long imm, int sub);

/* push/pop of a register mask. Always the 32-bit form, whose mask this
 * file's caller PATCHES once the body has shown which registers it
 * touched — hence the returned offset. */
int  t_push(struct code *c, unsigned mask);
int  t_pop(struct code *c, unsigned mask);
void t_patch_push(struct code *c, int at, unsigned mask);
void t_patch_pop(struct code *c, int at, unsigned mask);

/* ---- control flow ---------------------------------------------------- */

/* Each returns the offset of the instruction, to be patched once the
 * target is known. The wide forms always, so a patch never changes a
 * length and nothing downstream of it moves. */
int  t_b(struct code *c);                  /* b.w, +/-16MB */
int  t_bcond(struct code *c, int cond);    /* b<c>.w, +/-1MB */
int  t_bl(struct code *c);                 /* bl, +/-16MB */
void t_patch_b(struct code *c, int at, int target);
void t_patch_bcond(struct code *c, int at, int target);
void t_patch_bl(struct code *c, int at, int target);

void t_bx(struct code *c, int rm);
void t_blx(struct code *c, int rm);
void t_nop(struct code *c);

/* rd = <pc-relative address>, as the movw/movt pair a symbol reference
 * relocates through. Returns the offset of the movw; the movt follows it
 * immediately, so a caller that relocates both knows where each is. */
int t_mov_addr(struct code *c, int rd, unsigned long value);

/* An `it` block header: `cond` and a mask of up to three following
 * instructions. mask bit i (from the top) is 1 for "then". */
void t_it(struct code *c, int cond, int nthen, unsigned pattern);

/* The condition that inverts this one. */
int t_cond_invert(int cond);

#endif
