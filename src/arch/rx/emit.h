/* Renesas RX (RXv1, the RX600/RX610 core) instruction encoding,
 * little-endian.
 *
 * RX is a CISC: instructions are one to eight bytes, most operations come
 * in several encodings of different lengths, and the shortest one that
 * holds an operand is the one a real assembler picks. So the wrappers
 * below take what the instruction MEANS -- the operation, the registers,
 * the immediate, the displacement in bytes -- and choose the encoding,
 * always the shortest; no caller names a form. The few that must have a
 * fixed length say so in their names (rx_mov_abs, the branch
 * placeholders), because a relocation or a later patch is aimed at a byte
 * offset inside them.
 *
 * The encodings were taken from QEMU's decoder (target/rx/insns.decode)
 * and the RX Family Software Manual, and tools/rxcheck checks every form
 * against QEMU's own disassembler (tests/golden/rx-encoding.sh) -- and,
 * where an rx-elf binutils is installed, byte for byte against GNU as.
 *
 * Three facts shape the interface:
 *
 *  - An immediate is stored in the fewest bytes that sign-extend back to
 *    it (the `li` field: 1, 2, 3 or 4 bytes), and many operations have a
 *    still shorter form for 0..15 (#uimm4) or 0..255 (#uimm8).
 *  - A memory displacement is UNSIGNED and SCALED by the access size:
 *    `mov.l 1020[r1], r2` stores 255 in one byte. A displacement that is
 *    negative, not a multiple of the size, or beyond 65535 units cannot
 *    be encoded at all; rx_dsp_ok says which, and the code generator
 *    computes such an address into a register first.
 *  - Branch displacements are measured from the branch instruction's own
 *    first byte, not from the next one.
 *
 * Every wrapper checks its operands and stops with an internal error
 * rather than truncate one.
 */
#ifndef EMBCC_ARCH_RX_EMIT_H
#define EMBCC_ARCH_RX_EMIT_H

#include "../code.h"

/* The sixteen general registers. r0 is the stack pointer (the ISP or USP,
 * as PSW.U selects). */
enum {
    RX_R0 = 0, RX_SP = 0, RX_R1, RX_R2, RX_R3, RX_R4, RX_R5, RX_R6, RX_R7,
    RX_R8, RX_R9, RX_R10, RX_R11, RX_R12, RX_R13, RX_R14, RX_R15
};

/* Access sizes, as the instruction fields number them. */
enum { RX_B = 0, RX_W = 1, RX_L = 2 };

/* The condition codes of BCnd, SCCnd and BMCnd, in the field's order.
 * After `cmp src, src2` (flags of src2 - src): GEU is C set, LTU is C
 * clear -- RX's carry is NOT a borrow, as on ARM. */
enum {
    RX_EQ = 0, RX_NE = 1, RX_GEU = 2, RX_LTU = 3, RX_GTU = 4, RX_LEU = 5,
    RX_PZ = 6, RX_N = 7, RX_GE = 8, RX_LT = 9, RX_GT = 10, RX_LE = 11,
    RX_O = 12, RX_NO = 13, RX_ALWAYS = 14
};
int rx_cond_invert(int cond);
const char *rx_cond_name(int cond);   /* "eq", "ne", "c", "nc", ... as GNU as */

/* ---- register to register: OP rs, rd ---------------------------------
 *
 * Two-operand: rd = rd OP rs, except CMP and TST (flags only, of
 * rd - rs and rd & rs), NEG/NOT/ABS (rd = OP rs), MOV (rd = rs),
 * EMUL/EMULU (rd+1:rd = rd * rs, 64 bits; rd at most r14), the shifts
 * and rotates (rd shifted by rs's low five bits), REVL/REVW (rd = the
 * bytes of rs reversed in the word / in each halfword) and XCHG. */
enum rx_op {
    RX_MOV, RX_ADD, RX_SUB, RX_CMP, RX_AND, RX_OR, RX_XOR, RX_TST, RX_MUL,
    RX_ADC, RX_SBB, RX_DIV, RX_DIVU, RX_EMUL, RX_EMULU, RX_MAX, RX_MIN,
    RX_NEG, RX_NOT, RX_ABS, RX_SHLL, RX_SHLR, RX_SHAR, RX_ROTL, RX_ROTR,
    RX_REVL, RX_REVW, RX_XCHG, RX_STZ, RX_STNZ
};
const char *rx_op_name(int op);
void rx_rr(struct code *c, int op, int rs, int rd);

/* NEG, NOT and ABS of a register in place: the two-byte forms. */
void rx_r(struct code *c, int op, int rd);

/* ---- an immediate: OP #imm, rd ----------------------------------------
 *
 * MOV ADD SUB CMP AND OR XOR TST MUL ADC DIV DIVU EMUL EMULU MAX MIN
 * STZ STNZ. `imm` is the 32-bit value (a long whose low 32 bits are
 * used, sign- or zero-extended alike). SUB has only a #uimm4 form, so a
 * larger subtrahend is ADD of its negation -- the same flags except C,
 * which callers that need it do not use with SUB #imm. */
void rx_ri(struct code *c, int op, long imm, int rd);
int  rx_ri_len(int op, long imm);
/* mov.l #imm32, rd in its six-byte form whatever the value, for a
 * relocated address: returns the offset of the four-byte field. */
int  rx_mov_abs(struct code *c, int rd, unsigned long imm);

/* ---- three operands ----------------------------------------------------
 * rd = rs2 OP rs for ADD, SUB (rd = rs2 - rs, the manual's
 * `sub src, src2, dest`), AND, OR, MUL. */
void rx_rrr(struct code *c, int op, int rs, int rs2, int rd);
/* rd = rs + imm (`add #imm, rs, rd`), any 32-bit imm. */
void rx_add3(struct code *c, long imm, int rs, int rd);
int  rx_add3_len(long imm, int rs, int rd);

/* ---- shifts by a constant ---------------------------------------------
 * SHLL SHLR SHAR #n, rs, rd (n 0..31; the two-byte form when rs == rd),
 * ROTL ROTR #n, rd. */
void rx_shift_i(struct code *c, int op, int n, int rs, int rd);

/* ---- extensions ---------------------------------------------------------
 * mov.b/mov.w rs, rd (sign-extend), movu.b/movu.w rs, rd (zero). */
void rx_ext(struct code *c, int size, int sign, int rs, int rd);

/* ---- memory -------------------------------------------------------------
 * Loads extend: mov.b/mov.w sign-extend, movu.b/movu.w zero-extend.
 * `dsp` is in BYTES. */
int  rx_dsp_ok(int size, long dsp);
/* ...and for a load (movu's dsp:16 limit) and a store of an immediate. */
int  rx_load_ok(int size, int sign, long dsp);
int  rx_store_imm_ok(int size, long dsp);
void rx_load(struct code *c, int size, int sign, long dsp, int rs, int rd);
void rx_store(struct code *c, int size, int rs, long dsp, int rd);
/* mov.size #imm, dsp[rd]: the value's low `size` bytes stored. */
void rx_store_imm(struct code *c, int size, long imm, long dsp, int rd);
/* [ri, rb]: the address is rb + ri * the access size. */
void rx_load_idx(struct code *c, int size, int sign, int ri, int rb, int rd);
void rx_store_idx(struct code *c, int size, int rs, int ri, int rb);
/* OP dsp[rs].size, rd for ADD SUB CMP AND OR XOR TST MUL MAX MIN DIV
 * DIVU EMUL EMULU (the memory form, `memex`): the source read from
 * memory and extended as `.ub`, `.w`, `.uw`, `.l` say (size and sign). */
void rx_rm(struct code *c, int op, int size, int sign, long dsp, int rs,
           int rd);
int  rx_rm_ok(int op, int size, int sign, long dsp);

/* ---- the stack ---------------------------------------------------------- */
void rx_push(struct code *c, int rs);
void rx_pop(struct code *c, int rd);
void rx_pushm(struct code *c, int rs, int rs2);    /* rs < rs2, rs >= 1 */
void rx_popm(struct code *c, int rd, int rd2);
void rx_rts(struct code *c);
/* rtsd #bytes: sp += bytes, then return. bytes a multiple of 4, <= 1020. */
void rx_rtsd(struct code *c, long bytes);
/* rtsd #bytes, rd-rd2: sp += bytes - 4 * (rd2 - rd + 1), pop rd..rd2,
 * return. `bytes` counts the popped registers too. */
void rx_rtsd_m(struct code *c, long bytes, int rd, int rd2);

/* ---- transfers ------------------------------------------------------------ */
void rx_jmp(struct code *c, int rs);
void rx_jsr(struct code *c, int rs);
void rx_bra_l(struct code *c, int rs);      /* pc += rs */
void rx_bsr_l(struct code *c, int rs);
/* bsr.a with a zero displacement, for a relocation (R_RX_DIR24S_PCREL at
 * +1): returns the instruction's offset. */
int  rx_bsr_a(struct code *c);
/* bra.a likewise (a tail call). */
int  rx_bra_a(struct code *c);

/* Branch placeholders, patched by rx_patch_branch once the target is
 * known. The kinds, and what each reaches (bytes from the branch):
 *   RX_BR_S  1 byte: bra.s, or beq.s/bne.s only     3 .. 10
 *   RX_BR_B  2 bytes: bCND.b / bra.b                -128 .. 127
 *   RX_BR_W  3 bytes: bra.w, or beq.w/bne.w only    -32768 .. 32767
 *   RX_BR_A  4 bytes: bra.a only                    -2^23 .. 2^23-1
 * `cond` is RX_ALWAYS for an unconditional branch. */
enum { RX_BR_S, RX_BR_B, RX_BR_W, RX_BR_A };
int  rx_branch(struct code *c, int kind, int cond);
int  rx_branch_len(int kind);
int  rx_branch_ok(int kind, int cond);           /* does the form exist */
int  rx_branch_reaches(int kind, long disp);
/* Point the branch at `at` to `target` (offsets in c). 0 if it does not
 * reach -- the caller takes a longer form. */
int  rx_patch_branch(struct code *c, int at, int target);
/* A branch with its displacement, for rxcheck and the asm vocabulary. */
void rx_branch_d(struct code *c, int kind, int cond, long disp);
void rx_bsr_w_d(struct code *c, long disp);
void rx_bsr_a_d(struct code *c, long disp);

/* ---- flags into a register ---------------------------------------------- */
void rx_scc(struct code *c, int cond, int rd);     /* scCND.l rd: 1 or 0 */

/* ---- control ------------------------------------------------------------ */
void rx_nop(struct code *c);
void rx_brk(struct code *c);
void rx_wait(struct code *c);
void rx_rte(struct code *c);
void rx_int(struct code *c, int n);
/* The PSW flag letters of setpsw/clrpsw: 0 C, 1 Z, 2 S, 3 O, 8 I, 9 U. */
void rx_setpsw(struct code *c, int bit);
void rx_clrpsw(struct code *c, int bit);
/* The control registers: 0 PSW, 1 PC (mvfc only), 2 USP, 3 FPSW,
 * 8 BPSW, 9 BPC, 10 ISP, 11 FINTV, 12 INTB. */
void rx_mvtc(struct code *c, int rs, int cr);
void rx_mvtc_i(struct code *c, long imm, int cr);
void rx_mvfc(struct code *c, int cr, int rd);
void rx_mvtipl(struct code *c, int ipl);

/* rolc/rorc rd: rotate left/right one bit through C. */
void rx_rolc(struct code *c, int rd);
void rx_rorc(struct code *c, int rd);
/* smovf: copy r3 bytes from [r2] to [r1] upward (r1, r2 advance, r3 to
 * 0); sstr.b: store r2's low byte r3 times from [r1]. */
void rx_smovf(struct code *c);
void rx_sstr_b(struct code *c);
/* push/pop a control register (pushc psw / popc psw: the whole PSW, the
 * interrupt mask and the flags with it). */
void rx_pushc(struct code *c, int cr);
void rx_popc(struct code *c, int cr);

/* ---- bits ------------------------------------------------------------- */
/* BSET BCLR BTST BNOT #bit, rd (bit 0..31). */
enum { RX_BSET, RX_BCLR, RX_BTST, RX_BNOT };
void rx_bit_i(struct code *c, int op, int bit, int rd);
/* bmCND #bit, rd: the bit set when the condition holds, else cleared. */
void rx_bmcnd(struct code *c, int cond, int bit, int rd);

/* The shortest `mov.l #imm, rd`'s length, and the number of bytes an
 * immediate's li field takes (1..4). */
int rx_li_bytes(long imm);

#endif
