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
/* op rd, rn, rm, <type> #amount -- the 32-bit data-processing form with a
 * shifted second operand: `add.w r0, r1, r2, lsl #2`. Never the 16-bit
 * encodings, which have no shift field. amount is 1..31 (LSR/ASR by 32
 * are not made here). */
void t_alu_reg_shift(struct code *c, int op, int rd, int rn, int rm,
                     int type, int amount, int s);
/* rd = rn's bits lsb .. lsb+width-1, at the bottom, zero- or sign-extended:
 * ubfx / sbfx. width is 1..32-lsb. */
void t_bfx(struct code *c, int rd, int rn, int lsb, int width, int sign);

/* ---- moves ---------------------------------------------------------- */

/* rd = rm. Two bytes for any pair of registers (the T1 form reaches the
 * high ones through its H bits) and it never touches the flags. */
void t_mov_reg(struct code *c, int rd, int rm);

/* rd = imm, whatever the constant. One halfword-pair `movw` below 65536,
 * `movw`+`movt` above, and the 16-bit `movs` when the value fits eight
 * bits and the caller says the flags are dead. */
void t_mov_imm(struct code *c, int rd, long imm, int s);
/* ...and the same where the flags are dead, which lets the two-byte
 * `movs` be chosen. See the note on the implementation. */
void t_mov_imm_dead_flags(struct code *c, int rd, long imm);

/* rd = ~rm. */
void t_mvn_reg(struct code *c, int rd, int rm, int s);
/* MOVS, which SETS the flags -- what an asm `movs` asks for. t_mov_imm's
 * `s` is a preference the wide and movw forms ignore. The immediate form
 * returns -1, emitting nothing, for a value no flag-setting MOV encodes. */
void t_movs_reg(struct code *c, int rd, int rm);
int t_movs_imm(struct code *c, int rd, long imm);

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
/* rd = rm with the bytes of each halfword swapped. */
void t_rev16(struct code *c, int rd, int rm);

/* ---- memory --------------------------------------------------------- */

/* rt = [rn + off] / [rn + off] = rt. `off` may be any value: the encoder
 * picks the scaled unsigned form, the signed 8-bit form or the 12-bit
 * form, and refuses (returns 0, writes nothing) when none reaches — the
 * caller must then materialise the address itself. */
int t_ldst_imm(struct code *c, int rt, int rn, long off, int size, int sign,
               int store);

/* ldrd / strd: rt = [rn + off], rt2 = [rn + off + 4], in one four-byte
 * instruction. `off` a multiple of 4 within +-1020; rt and rt2 neither sp
 * nor pc, and different for a load. Returns 0, writing nothing, when the
 * form cannot say it. The address must be word-aligned: ARMv7-M faults on
 * an unaligned ldrd/strd where two ldr would not. */
int t_ldst_pair(struct code *c, int rt, int rt2, int rn, long off, int store);
/* With writeback: pre != 0 is [rn, #off]!, 0 is [rn], #off. 0 when it
 * cannot be encoded (|off| > 255, rn pc or rt). */
int t_ldst_wb(struct code *c, int rt, int rn, long off, int size, int sign,
              int store, int pre);

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
/* adr.w rd, Align(pc, 4) + imm12 (the ADD form, T3): a jump table's
 * address, which sits a known few bytes on. */
int  t_adr_w(struct code *c, int rd, int imm12);           /* where it is */
void t_patch_adr_w(struct code *c, int at, int rd, int imm12);
/* tbh [pc, rm, lsl #1]: pc += 2 * halfword[rm] of the table that follows
 * (pc is the tbh's address + 4, i.e. the table). Returns where it is. */
int  t_tbh(struct code *c, int rm);
void t_patch_hw16(struct code *c, int at, unsigned v);     /* one halfword */
void t_blx(struct code *c, int rm);
void t_nop(struct code *c);

/* One half of an address: movw (top=0) or movt (top=1). */
void t_movw_movt(struct code *c, int rd, unsigned v, int top);

/* rd = <pc-relative address>, as the movw/movt pair a symbol reference
 * relocates through. Returns the offset of the movw; the movt follows it
 * immediately, so a caller that relocates both knows where each is. */
int t_mov_addr(struct code *c, int rd, unsigned long value);

/* An `it` block header: `cond` for the first instruction, then `te`, one
 * letter per further instruction ("" for `it`, "e" for `ite`, "tt" for
 * `ittt`): 't' runs under cond, 'e' under its inverse. At most three. */
void t_it(struct code *c, int cond, const char *te);
void t_setcc_low(struct code *c, int cond, int rd);

/* The condition that inverts this one. */
int t_cond_invert(int cond);

/* ---- the system instructions ------------------------------------------
 *
 * What a Cortex-M program reaches inline assembly FOR: the special
 * registers, the interrupt masks, the barriers and the hints. None of
 * them is reachable from C, which is why every CMSIS header opens with
 * an asm block.
 *
 * Each is derived from its field layout rather than restated as a
 * constant, and tools/thumbcheck round-trips all of them through
 * llvm-objdump -- the same discipline the rest of this file is under.
 */

/* The M-profile special registers, by their SYSm number. */
enum {
    T_SYS_APSR = 0, T_SYS_IAPSR = 1, T_SYS_EAPSR = 2, T_SYS_XPSR = 3,
    T_SYS_IPSR = 5, T_SYS_EPSR = 6, T_SYS_IEPSR = 7,
    T_SYS_MSP = 8, T_SYS_PSP = 9,
    T_SYS_PRIMASK = 16, T_SYS_BASEPRI = 17, T_SYS_BASEPRI_MAX = 18,
    T_SYS_FAULTMASK = 19, T_SYS_CONTROL = 20
};

void t_mrs(struct code *c, int rd, int sysm);
void t_msr(struct code *c, int sysm, int rn);

/* `cpsid`/`cpsie` over the i and f masks. Masking interrupts is what a
 * critical section is on this machine. */
void t_cps(struct code *c, int disable, int mask_i, int mask_f);

/* Barriers. `op` is 4 for dsb, 5 for dmb, 6 for isb -- the field's own
 * numbering, so the three share one encoder. */
enum { T_BAR_DSB = 4, T_BAR_DMB = 5, T_BAR_ISB = 6 };
void t_barrier(struct code *c, int op);

/* Hints, likewise by their field value. nop is here too, and t_nop is
 * the same instruction under the name the rest of this file uses. */
enum { T_HINT_NOP = 0, T_HINT_YIELD = 1, T_HINT_WFE = 2,
       T_HINT_WFI = 3, T_HINT_SEV = 4 };
void t_hint(struct code *c, int op);

void t_bkpt(struct code *c, int imm8);

/* SVC #imm8: the supervisor call an RTOS enters its kernel through. */
void t_svc(struct code *c, int imm8);

/* TST rn, #imm: AND setting the flags into nothing. 0 when imm is not a
 * modified immediate. */
int t_tst_imm(struct code *c, int rn, long imm);

/* LDM/STM rn{!}, {list}, always the 32-bit form: `before` is DB (else IA),
 * `load` LDM (else STM). 0 when the list is one the architecture forbids:
 * fewer than two registers, sp in it, pc in a store's, pc and lr both in a
 * load's, or rn in it with writeback. */
int t_ldm_stm(struct code *c, int rn, unsigned mask, int wback, int before,
              int load);

/* VLDM/VSTM rn{!}, {s<first>-s<first+n-1>}: `before` is DB (which needs
 * writeback), else IA. 0 when it is not encodable. */
int t_vldm_vstm(struct code *c, int rn, int first, int n, int wback,
                int before, int load);

/* LDR rt, [pc, #off], `off` from Align(pc, 4), always the 32-bit form so
 * its length never depends on the offset. 0 when |off| > 4095. */
int t_ldr_lit(struct code *c, int rt, long off);

/* ---- VFP (FPv4-SP-D16, the Cortex-M4F unit) ----
 *
 * `dbl` selects the width: 0 for a single (s0-s31), 1 for a double
 * (d0-d15). The register NUMBERING differs between them and vsplit in
 * emit.c is the one place that knows how -- see the note there, because
 * getting it backwards names a different register and still assembles.
 *
 * Single precision is what the hardware computes. A double has
 * registers and moves but no arithmetic on this part, so t_vadd(.., 1)
 * exists for an M7 and for the ABI's sake, not because M4F can use it. */
void t_vadd(struct code *c, int d, int n, int m, int dbl);
void t_vsub(struct code *c, int d, int n, int m, int dbl);
void t_vmul(struct code *c, int d, int n, int m, int dbl);
void t_vdiv(struct code *c, int d, int n, int m, int dbl);
void t_vfma(struct code *c, int d, int n, int m, int dbl);
void t_vmov_reg(struct code *c, int d, int m, int dbl);
void t_vabs(struct code *c, int d, int m, int dbl);
void t_vneg(struct code *c, int d, int m, int dbl);
void t_vsqrt(struct code *c, int d, int m, int dbl);
void t_vcmp(struct code *c, int n, int m, int dbl);
void t_vcmpe(struct code *c, int n, int m, int dbl);
void t_vpush_s(struct code *c, int first, int n, int pop);
void t_ldrexbh(struct code *c, int rt, int rn, int size);
void t_strexbh(struct code *c, int rd, int rt, int rn, int size);
void t_clrex(struct code *c);
int t_bcond16(struct code *c, int cond);
int t_b16(struct code *c);
int t_cbz(struct code *c, int nonzero, int rn);
int t_patch_cbz(struct code *c, int at, int target);
int t_patch_bcond16(struct code *c, int at, int target);
int t_patch_b16(struct code *c, int at, int target);
/* LDR (literal) T1, two bytes; 0 when rt or off does not fit it. */
int t_ldr_lit16(struct code *c, int rt, long off);
/* An assembly file's `ldr rd, =v` without a literal (mov.w, mvn.w,
 * movw); 0 when it needs one. */
int t_ldr_const(struct code *c, int rd, unsigned long v);
void t_vcvt_f_from_i(struct code *c, int d, int m, int sgn, int dbl);
void t_vcvt_i_from_f(struct code *c, int d, int m, int sgn, int dbl);
void t_vldst(struct code *c, int sd, int rn, int off, int dbl, int store);
void t_vmov_core(struct code *c, int sn, int rt, int to_fp);
void t_vmov_core_pair(struct code *c, int dm, int rt, int rt2, int to_fp);
void t_vmrs_apsr(struct code *c);
void t_rbit(struct code *c, int rd, int rm);

/* The exclusive pair, which is how an atomic is built here: `off` is a
 * byte offset and must be a multiple of four. */
void t_ldrex(struct code *c, int rt, int rn, int off);
void t_strex(struct code *c, int rd, int rt, int rn, int off);

#endif
