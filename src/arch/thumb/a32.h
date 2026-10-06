/* The A32 (ARM state) encoder: the same operations as emit.h's t_*
 * encoders, written as ARM instructions, for armv7a-none-eabi.
 *
 * Not called directly by the code generator. Every t_* encoder in emit.c
 * begins by handing its call here when `t_isa_a32` is set, so the ARMv7-M
 * instruction selection in codegen.c -- and the inline-asm assembler --
 * emit A32 without knowing it (docs/internals/arm-a32-plan.md says why).
 * Each a32_* function has its t_* twin's meaning, described in emit.h;
 * where A32 cannot say what the Thumb form can, the difference is noted
 * at the function and the encoder answers the way emit.h's contract
 * allows: 0 (nothing written) from the int-returning ones, an internal
 * error from the rest -- never a different instruction.
 *
 * Every instruction is one little-endian word. The condition field comes
 * from the IT queue (a32_it): t_it emits nothing in ARM state, it says
 * which condition each of the next calls runs under, and every call
 * takes one entry -- all the words that call writes carry it. Outside an
 * IT queue the condition is AL.
 *
 * tools/a32check prints every form with the word made for it, and
 * tests/golden/arm-a32-encoding.sh has llvm-mc encode the same text.
 */
#ifndef EMBCC_ARCH_THUMB_A32_H
#define EMBCC_ARCH_THUMB_A32_H

#include "../code.h"

void a32_alu_reg_shift(struct code *c, int op, int rd, int rn, int rm,
                       int type, int amount, int s);
void a32_bfx(struct code *c, int rd, int rn, int lsb, int width, int sign);
void a32_mov_reg(struct code *c, int rd, int rm);
void a32_mov_imm(struct code *c, int rd, long imm, int s);
void a32_mvn_reg(struct code *c, int rd, int rm, int s);
void a32_movs_reg(struct code *c, int rd, int rm);
int  a32_movs_imm(struct code *c, int rd, long imm);
void a32_alu_reg(struct code *c, int op, int rd, int rn, int rm, int s);
int  a32_alu_imm(struct code *c, int op, int rd, int rn, long imm, int s);
int  a32_imm_ok(long imm);
/* The 12-bit field of an A32 modified immediate that means `v`, or -1. */
int  a32_encode_imm(unsigned long v);
void a32_addsubw(struct code *c, int rd, int rn, long imm, int sub);
void a32_shift_imm(struct code *c, int op, int rd, int rm, int sh, int s);
void a32_shift_reg(struct code *c, int op, int rd, int rn, int rm, int s);
void a32_mul(struct code *c, int rd, int rn, int rm);
void a32_mla(struct code *c, int rd, int rn, int rm, int ra, int sub);
void a32_div(struct code *c, int rd, int rn, int rm, int sign);
void a32_mull(struct code *c, int rdlo, int rdhi, int rn, int rm, int sign);
void a32_cmp_reg(struct code *c, int rn, int rm);
void a32_cmp_imm(struct code *c, int rn, long imm);
void a32_tst_reg(struct code *c, int rn, int rm);
int  a32_tst_imm(struct code *c, int rn, long imm);
void a32_ext(struct code *c, int rd, int rm, int size, int sign);
/* clz, rev, rev16, rbit: rd = f(rm). */
enum { A32_CLZ, A32_REV, A32_REV16, A32_RBIT };
void a32_bitop(struct code *c, int which, int rd, int rm);

int  a32_ldst_imm(struct code *c, int rt, int rn, long off, int size,
                  int sign, int store);
int  a32_ldst_pair(struct code *c, int rt, int rt2, int rn, long off,
                   int store);
int  a32_ldst_wb(struct code *c, int rt, int rn, long off, int size,
                 int sign, int store, int pre);
/* [rn, rm, lsl #shift]: a word or an unsigned byte with any shift; a
 * halfword or a signed load only with shift 0 (A32's "extra" loads and
 * stores have no shift). a32_ldst_reg_ok says which. */
int  a32_ldst_reg_ok(int shift, int size, int sign, int store);
void a32_ldst_reg(struct code *c, int rt, int rn, int rm, int shift,
                  int size, int sign, int store);
void a32_add_sp(struct code *c, int rd, long off);
void a32_sp_adjust(struct code *c, long imm, int sub);
int  a32_push(struct code *c, unsigned mask);
int  a32_pop(struct code *c, unsigned mask);
void a32_patch_mask(struct code *c, int at, unsigned mask);
int  a32_ldm_stm(struct code *c, int rn, unsigned mask, int wback,
                 int before, int load);

/* b / bl / b<cond>, each one word reaching +-32 MB from the instruction's
 * address plus eight. Emitted with a zero offset; the patch fills it and
 * refuses (0) one that does not reach or is not a multiple of four. */
int  a32_b(struct code *c, int cond, int link);
int  a32_patch_b(struct code *c, int at, int target);
void a32_bx(struct code *c, int rm, int link);
void a32_nop(struct code *c);
void a32_movw_movt(struct code *c, int rd, unsigned v, int top);
int  a32_mov_addr(struct code *c, int rd, unsigned long value);
/* rd = pc + 8 + imm (imm may be negative): ADR. 0 when imm does not
 * rotate. a32_patch_adr re-aims one at `at`. */
int  a32_adr(struct code *c, int rd, long imm);
int  a32_patch_adr(struct code *c, int at, int rd, long imm);

/* The IT queue: `te` as t_it takes it. */
void a32_it(int cond, const char *te);
/* 1 while an IT queue still has conditions to hand out. */
int  a32_it_open(void);
void a32_it_reset(void);
void a32_setcc(struct code *c, int cond, int rd);

void a32_cps(struct code *c, int disable, int mask_i, int mask_f);
void a32_barrier(struct code *c, int op);
void a32_clrex(struct code *c);
void a32_hint(struct code *c, int op);
void a32_bkpt(struct code *c, int imm);
void a32_svc(struct code *c, long imm);
void a32_udf(struct code *c, int imm);
/* MRS rd, CPSR / MSR CPSR_<fields>, rn: `fields` the 4-bit mask c,x,s,f
 * (bit 0 c). */
void a32_mrs_cpsr(struct code *c, int rd);
void a32_msr_cpsr(struct code *c, int fields, int rn);
/* MRC/MCR p<cp>, #opc1, rt, c<crn>, c<crm>, #opc2. */
void a32_mrc_mcr(struct code *c, int load, int cp, int opc1, int rt,
                 int crn, int crm, int opc2);
int  a32_ldr_lit(struct code *c, int rt, long off);
int  a32_ldr_const(struct code *c, int rd, unsigned long v);

/* ldrex/strex: size 1, 2, 4, or 8 for the doubleword pair (rt even,
 * rt2 = rt + 1). strex writes 0 to rd on success. */
void a32_ldrex(struct code *c, int rt, int rn, int size);
void a32_strex(struct code *c, int rd, int rt, int rn, int size);

/* A Thumb-2 VFP/coprocessor instruction's two halfwords (1110 xxxx...):
 * the A32 word is the same 28 bits under a condition field. */
void a32_vfp_word(struct code *c, unsigned h1, unsigned h2);

#endif
