/* Xtensa instruction encoding, little-endian (the ESP32's LX6 and LX7).
 *
 * Nearly every instruction is 24 bits -- three bytes, low byte first --
 * in one of a handful of formats that all keep op0 in bits 3..0 and the
 * register fields t, s, r in 7..4, 11..8 and 15..12:
 *
 *   RRR    op0 t s r op1 op2       the register-register group
 *   RRI8   op0 t s r imm8          loads, stores, addi, two-register branches
 *   RI16   op0 t imm16             l32r
 *   CALL   op0 n offset18          call0/4/8/12 and j
 *   BRI8   op0 n m s r imm8        a register and a 4-bit constant code
 *   BRI12  op0 n m s imm12         beqz/bnez/bltz/bgez and entry
 *   RSR    op0 t sr op1 op2        rsr/wsr/xsr, sr in the r and s fields
 *
 * This file packs those formats once and every instruction is its opcode
 * fields plus which operand goes where (the RISC-V and MIPS encoders'
 * rule), so a new instruction cannot put a field in the wrong place.
 *
 * What makes Xtensa easy to get wrong is the immediates, which are almost
 * all something other than the plain number: slli encodes 32 - n, srli
 * reaches only 15, the load offsets are scaled by the access size and
 * unsigned, addmi takes a multiple of 256, the immediate branches take a
 * 4-bit INDEX into a table of sixteen constants (b4const, b4constu), l32r
 * reaches only backwards, entry's frame is in units of 8, and l32e/s32e
 * take a negative offset. Each wrapper checks the range its field covers
 * and stops with an internal error rather than truncate: a truncated field
 * is a valid different instruction.
 *
 * The density option's 16-bit forms are not emitted (yet); every
 * instruction here is three bytes.
 *
 * tools/xtensacheck lays every form out at a known address and QEMU's own
 * disassembler for the de212 core -- generated from the core's ISA
 * description, the same tables the processor is built from -- decodes it;
 * tests/golden/xtensa-encoding.sh compares the two, line by line.
 */
#ifndef EMBCC_ARCH_XTENSA_EMIT_H
#define EMBCC_ARCH_XTENSA_EMIT_H

#include "../code.h"

/* The address registers as the current window shows them. Under the
 * windowed ABI a0 is the return address (with the caller's window
 * increment in its top two bits), a1 the stack pointer, a2-a7 the
 * incoming arguments, and a8-a15 the registers a call8 hands to its
 * callee (the outgoing arguments start at a10). */
enum {
    XT_A0 = 0, XT_A1, XT_A2, XT_A3, XT_A4, XT_A5, XT_A6, XT_A7,
    XT_A8, XT_A9, XT_A10, XT_A11, XT_A12, XT_A13, XT_A14, XT_A15
};
#define XT_SP XT_A1

/* Special registers by number, as rsr/wsr/xsr name them. */
enum {
    XT_SR_LBEG = 0, XT_SR_LEND = 1, XT_SR_LCOUNT = 2, XT_SR_SAR = 3,
    XT_SR_SCOMPARE1 = 12, XT_SR_ACCLO = 16, XT_SR_ACCHI = 17,
    XT_SR_WINDOWBASE = 72, XT_SR_WINDOWSTART = 73,
    XT_SR_ATOMCTL = 99,
    XT_SR_EPC1 = 177, XT_SR_DEPC = 192, XT_SR_EXCSAVE1 = 209,
    XT_SR_INTENABLE = 228, XT_SR_PS = 230, XT_SR_VECBASE = 231,
    XT_SR_EXCCAUSE = 232, XT_SR_CCOUNT = 234, XT_SR_PRID = 235,
    XT_SR_EXCVADDR = 238
};

/* The name ("a0".."a15") of a register, for -S and diagnostics. */
const char *xt_reg_name(int r);

/* ---- the formats ------------------------------------------------------ */

unsigned long xt_enc_rrr(int op0, int op1, int op2, int r, int s, int t);
unsigned long xt_enc_rri8(int op0, int r, int s, int t, unsigned imm8);
unsigned long xt_enc_ri16(int op0, int t, unsigned imm16);
unsigned long xt_enc_call(int op0, int n, unsigned long off18);
unsigned long xt_enc_bri8(int op0, int n, int m, int s, int r, unsigned imm8);
unsigned long xt_enc_bri12(int op0, int n, int m, int s, unsigned imm12);

/* Every instruction becomes three little-endian bytes here. */
void xt_w(struct code *c, unsigned long w);
/* The 24-bit word at `at`, and a rewrite of it. */
unsigned long xt_get(const struct code *c, int at);
void xt_put(struct code *c, int at, unsigned long w);

/* ---- moves and constants ------------------------------------------------ */

void xt_mov(struct code *c, int d, int s);           /* or d, s, s */
/* movi t, imm: a SIGNED 12-bit value, -2048..2047. */
void xt_movi(struct code *c, int t, long imm);
int xt_movi_ok(long long v);
/* t = v with no literal: movi, or movi and one slli, or movi, slli and
 * an addi/addmi -- whatever covers v in at most three instructions.
 * Returns 0 and emits nothing when none does; the caller then loads v
 * from a literal (l32r). */
int xt_li_inline(struct code *c, int t, long long v);
int xt_li_inline_len(long long v);   /* bytes, or 0 where it needs a literal */

/* ---- arithmetic and logic --------------------------------------------------
 *
 * r = s OP t, in source order whatever the field order. The add/sub
 * scaled forms are (s << k) +/- t. The conditional moves write r = s when
 * t tests (== 0, != 0, < 0, >= 0) and leave r alone otherwise. src is the
 * funnel shift: r = (s:t) >> SAR, the 64-bit concatenation with s high. */
enum xt_alu {
    XT_ADD, XT_SUB, XT_AND, XT_OR, XT_XOR,
    XT_ADDX2, XT_ADDX4, XT_ADDX8, XT_SUBX2, XT_SUBX4, XT_SUBX8,
    XT_MULL, XT_MUL16U, XT_MUL16S, XT_QUOS, XT_QUOU, XT_REMS, XT_REMU,
    XT_MIN, XT_MAX, XT_MINU, XT_MAXU,
    XT_MOVEQZ, XT_MOVNEZ, XT_MOVLTZ, XT_MOVGEZ,
    XT_SRC
};
void xt_alu(struct code *c, int op, int r, int s, int t);
void xt_neg(struct code *c, int r, int t);
void xt_abs(struct code *c, int r, int t);
/* addi: -128..127. addmi: a multiple of 256 in -32768..32512. */
void xt_addi(struct code *c, int t, int s, long imm);
void xt_addmi(struct code *c, int t, int s, long imm);
/* t = s + imm with no literal, in one or two instructions (addi, addmi,
 * or addmi then addi); 0 and nothing emitted when imm needs more. */
int xt_addi_any(struct code *c, int t, int s, long long imm);
int xt_addi_any_len(long long imm);

/* ---- shifts --------------------------------------------------------------
 *
 * The immediate shifts: slli by 1..31 (0 is not encodable: use mov),
 * srli by 0..15, srai by 0..31. A logical right shift by 16..31 is extui.
 * extui r, t, shift, bits takes `bits` (1..16) bits of t starting at bit
 * `shift` (0..31), zero-extended. The variable shifts read SAR, which
 * ssl (left: SAR = 32 - (s & 31)), ssr (right: SAR = s & 31), ssa8l
 * (SAR = (s & 3) * 8) and ssai (an immediate 0..31) set. */
void xt_slli(struct code *c, int r, int s, int n);
void xt_srli(struct code *c, int r, int t, int n);
void xt_srai(struct code *c, int r, int t, int n);
void xt_extui(struct code *c, int r, int t, int shift, int bits);
void xt_ssl(struct code *c, int s);
void xt_ssr(struct code *c, int s);
void xt_ssa8l(struct code *c, int s);
void xt_ssai(struct code *c, int n);
void xt_sll(struct code *c, int r, int s);
void xt_srl(struct code *c, int r, int t);
void xt_sra(struct code *c, int r, int t);

/* sext r, s, b: sign-extend from bit b (7..22). clamps: saturate to the
 * signed range of b + 1 bits. nsa/nsau: the normalisation shift, nsau of 0
 * being 32 -- a count of leading zeros. */
void xt_sext(struct code *c, int r, int s, int b);
void xt_clamps(struct code *c, int r, int s, int b);
void xt_nsa(struct code *c, int t, int s);
void xt_nsau(struct code *c, int t, int s);

/* ---- memory ----------------------------------------------------------------
 *
 * t = *(base + off) of 1, 2 or 4 bytes. The offset is UNSIGNED and scaled
 * by the size: 0..255, 0..510 even, 0..1020 a multiple of 4. A byte load
 * zero-extends; there is no signed byte load (l8ui then sext), so `sign`
 * with size 1 is an internal error. Halfwords have both (l16si, l16ui). */
void xt_load(struct code *c, int t, int base, long off, int size, int sign);
void xt_store(struct code *c, int t, int base, long off, int size);
/* Does `off` fit a size-byte access's field? */
int xt_mem_ok(long off, int size);
/* l32r t, at the literal `lit` (an offset in the same buffer, as the
 * instruction's own `at` is): the literal must be BELOW the instruction,
 * within 256 KiB of it, and 4-aligned. The address is computed from
 * (at + 3) rounded down to 4 -- it is the encoder's to do, given both. */
unsigned long xt_enc_l32r(int t, long at, long lit);
void xt_l32r(struct code *c, int t, long lit);
int xt_l32r_reaches(long at, long lit);
/* The acquire load, release store and compare-and-swap of the
 * multiprocessor-synchronisation and conditional-store options; offsets
 * as l32i's. s32c1i stores t when *(s+off) equals SCOMPARE1 and returns
 * the old value in t either way. */
void xt_l32ai(struct code *c, int t, int s, long off);
void xt_s32ri(struct code *c, int t, int s, long off);
void xt_s32c1i(struct code *c, int t, int s, long off);
/* The window-exception handlers' loads and stores: offset -64..-4. */
void xt_l32e(struct code *c, int t, int s, long off);
void xt_s32e(struct code *c, int t, int s, long off);

/* ---- control flow ----------------------------------------------------------
 *
 * Branch displacements are BYTES from the instruction after the branch,
 * i.e. target - (at + 4) -- including for a three-byte instruction, as
 * the ISA defines it. A two-register branch (RRI8) reaches -128..127,
 * a branch against zero (BRI12) -2048..2047, j -131072..131071. */
enum xt_cond {
    XT_BNONE, XT_BEQ, XT_BLT, XT_BLTU, XT_BALL, XT_BBC, XT_BANY, XT_BNE,
    XT_BGE, XT_BGEU, XT_BNALL, XT_BBS
};
unsigned long xt_enc_b(int cond, int s, int t, long off);
/* bbci/bbsi s, bit: branch on bit `bit` (0..31) clear / set. */
unsigned long xt_enc_bbi(int set, int s, int bit, long off);
/* Against zero: beqz, bnez, bltz, bgez. */
enum xt_zcond { XT_BEQZ, XT_BNEZ, XT_BLTZ, XT_BGEZ };
unsigned long xt_enc_bz(int zcond, int s, long off);
/* Against a constant: beqi, bnei, blti, bgei (k from b4const) and bltui,
 * bgeui (k from b4constu). The constants these can test are in the
 * tables; xt_bi_ok says whether one is. */
enum xt_icond { XT_BEQI, XT_BNEI, XT_BLTI, XT_BGEI, XT_BLTUI, XT_BGEUI };
int xt_bi_ok(int icond, long long k);
unsigned long xt_enc_bi(int icond, int s, long long k, long off);
unsigned long xt_enc_j(long off);
/* The reach of each form, for the code generator's relaxation. */
int xt_b_reaches(long off);       /* RRI8 and BRI8 */
int xt_bz_reaches(long off);      /* BRI12 */
int xt_j_reaches(long off);

/* Re-point the branch or j at `at` to `target`. Returns 0 when it does
 * not reach. */
int xt_patch_branch(struct code *c, int at, int target);

/* call0/4/8/12 (`n` the window increment / 4: 0, 1, 2, 3) with a zero
 * offset, for an R_XTENSA_SLOT0_OP relocation; the target of a call is
 * (at & ~3) + 4 + offset * 4, so a callee must start 4-aligned. And the
 * encoding of one whose target is known. */
void xt_call(struct code *c, int n);
unsigned long xt_enc_call_to(int n, long at, long target);
int xt_call_reaches(long at, long target);
void xt_callx(struct code *c, int n, int s);
void xt_jx(struct code *c, int s);
void xt_ret(struct code *c);
void xt_retw(struct code *c);
/* entry s, frame: frame a multiple of 8 in 0..32760. */
void xt_entry(struct code *c, int s, long frame);
void xt_movsp(struct code *c, int t, int s);
void xt_rotw(struct code *c, int n);              /* -8..7 */

/* ---- the system --------------------------------------------------------- */

void xt_nop(struct code *c);
void xt_ill(struct code *c);
void xt_break(struct code *c, int s, int t);      /* 0..15 each */
void xt_syscall(struct code *c);
void xt_simcall(struct code *c);
void xt_memw(struct code *c);
void xt_isync(struct code *c);
void xt_rsync(struct code *c);
void xt_esync(struct code *c);
void xt_dsync(struct code *c);
void xt_extw(struct code *c);
void xt_rfe(struct code *c);
void xt_rfde(struct code *c);
void xt_rfwo(struct code *c);
void xt_rfwu(struct code *c);
void xt_rfi(struct code *c, int level);           /* 1..15 */
void xt_rsil(struct code *c, int t, int level);   /* 0..15 */
void xt_waiti(struct code *c, int level);
void xt_rsr(struct code *c, int t, int sr);
void xt_wsr(struct code *c, int t, int sr);
void xt_xsr(struct code *c, int t, int sr);

/* The length of the instruction whose first byte is `b0`: 3, or 2 for a
 * density instruction (op0 8..13), which the decoders of -S and the
 * linker need although nothing here emits one. */
int xt_insn_len(int b0);

#endif
