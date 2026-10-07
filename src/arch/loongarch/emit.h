/* LoongArch64 instruction encoding (LA64, little-endian).
 *
 * Every instruction is one 32-bit word, and every one is a handful of
 * fields under an opcode that fills the rest: the register forms put rd
 * in bits 4:0, rj in 9:5 and rk in 14:10, and the immediate forms put a
 * 12-, 14-, 16- or 20-bit field above rj (or above rd). So this file
 * packs the FORMATS once -- 2R, 3R, 2RI12, 2RI14, 2RI16, 1RI20, 1RI21
 * and I26 -- and every instruction is its opcode and which value goes in
 * which field. A new instruction cannot put a field in the wrong place,
 * because it does not name the places (the RISC-V encoder's rule).
 *
 * The immediates are where LoongArch differs from RISC-V, and the
 * difference is the kind that encodes cleanly and computes something
 * else:
 *
 *   addi.w/addi.d, slti, sltui, the loads and the stores SIGN-extend
 *   their 12 bits (sltui sign-extends and then compares unsigned);
 *   andi, ori and xori ZERO-extend theirs -- `ori rd, zero, 4095` is
 *   4095, and there is no `andi rd, rj, -16`.
 *
 * Each wrapper checks the range of the extension its instruction
 * performs and stops with an internal error rather than truncate.
 *
 * Branch and jump offsets are BYTES here, from the branch itself, and a
 * multiple of four; the field holds them shifted right by two. beq-style
 * branches reach +-128 KiB, beqz/bnez +-4 MiB, b/bl +-128 MiB.
 *
 * tools/lacheck compares every form below with llvm-mc
 * (tests/golden/loongarch-encoding.sh).
 */
#ifndef EMBCC_ARCH_LOONGARCH_EMIT_H
#define EMBCC_ARCH_LOONGARCH_EMIT_H

#include "../code.h"

/* The register file, by psABI name. r21 is reserved by the psABI and
 * nothing here ever names it. */
enum {
    LA_ZERO = 0, LA_RA = 1, LA_TP = 2, LA_SP = 3,
    LA_A0 = 4, LA_A1 = 5, LA_A2 = 6, LA_A3 = 7,
    LA_A4 = 8, LA_A5 = 9, LA_A6 = 10, LA_A7 = 11,
    LA_T0 = 12, LA_T1 = 13, LA_T2 = 14, LA_T3 = 15, LA_T4 = 16,
    LA_T5 = 17, LA_T6 = 18, LA_T7 = 19, LA_T8 = 20,
    LA_R21 = 21,
    LA_FP = 22,                                 /* s9 */
    LA_S0 = 23, LA_S1 = 24, LA_S2 = 25, LA_S3 = 26, LA_S4 = 27,
    LA_S5 = 28, LA_S6 = 29, LA_S7 = 30, LA_S8 = 31
};

#define LA_NARGREG 8
extern const int la_argreg[LA_NARGREG];

/* The psABI name of a register ("zero", "ra", ... "s8"), for -S, inline
 * asm and diagnostics. */
const char *la_reg_name(int r);

/* ---- the formats ---------------------------------------------------- */

/* Each takes the opcode word (the instruction with every field zero, as
 * llvm-mc encodes it) and the FIELDS: an immediate is the field's bits,
 * already reduced to its width by the caller's wrapper. */
unsigned long la_enc_2r(unsigned long op, int rd, int rj);
unsigned long la_enc_3r(unsigned long op, int rd, int rj, int rk);
unsigned long la_enc_2ri12(unsigned long op, int rd, int rj, unsigned imm12);
unsigned long la_enc_2ri14(unsigned long op, int rd, int rj, unsigned imm14);
unsigned long la_enc_2ri16(unsigned long op, int rd, int rj, unsigned imm16);
unsigned long la_enc_1ri20(unsigned long op, int rd, unsigned imm20);
unsigned long la_enc_1ri21(unsigned long op, int rj, unsigned imm21);
unsigned long la_enc_i26(unsigned long op, unsigned imm26);

/* Does a value fit in `bits` signed (la_fits) or unsigned (la_ufits)? */
int la_fits(long long v, int bits);
int la_ufits(long long v, int bits);

/* The one place an instruction becomes bytes. */
void la_w(struct code *c, unsigned long w);

/* ---- arithmetic and logic ------------------------------------------- */

enum {
    LA_ADD, LA_SUB, LA_SLT, LA_SLTU, LA_AND, LA_OR, LA_XOR, LA_NOR,
    LA_ANDN, LA_ORN, LA_SLL, LA_SRL, LA_SRA, LA_ROTR, LA_MASKEQZ,
    LA_MASKNEZ, LA_MUL, LA_MULH, LA_MULHU, LA_DIV, LA_DIVU, LA_MOD,
    LA_MODU,
    LA_NALU
};

/* rd = rj <op> rk. `w` selects the .w form (a 32-bit operation whose
 * result is sign-extended to 64 bits), which exists for add, sub, the
 * shifts, rotr, the multiplies and the divides; for the others it is an
 * internal error. NB: div.w, mod.w, div.wu and mod.wu read their
 * operands as SIGN-EXTENDED 32-bit values and are undefined otherwise --
 * the caller makes sure they are. */
void la_alu(struct code *c, int op, int rd, int rj, int rk, int w);

/* rd = rj <op> imm, for the ops with a 12-bit immediate form: LA_ADD
 * (addi.w with w, addi.d without), LA_SLT (slti), LA_SLTU (sltui), all
 * three SIGNED; LA_AND, LA_OR, LA_XOR (andi, ori, xori), UNSIGNED
 * 0..4095. */
void la_alu_imm(struct code *c, int op, int rd, int rj, long long imm, int w);

/* A shift (LA_SLL, LA_SRL, LA_SRA, LA_ROTR) by a constant: 0..31 for the
 * .w forms, 0..63 for the .d ones. */
void la_shift_imm(struct code *c, int op, int rd, int rj, int amt, int w);

void la_mv(struct code *c, int rd, int rj);    /* or rd, rj, zero */
void la_nop(struct code *c);                   /* andi zero, zero, 0 */

/* ext.w.b / ext.w.h: sign-extend the low `size` (1 or 2) bytes. */
void la_ext(struct code *c, int rd, int rj, int size);
/* bstrpick: rd = rj[msb:lsb], zero-extended; `d` the .d form (bits up to
 * 63), else .w (up to 31, the result sign-extended from bit 31). */
void la_bstrpick(struct code *c, int rd, int rj, int msb, int lsb, int d);
/* alsl: rd = (rj << sa) + rk, sa 1..4; `d` the .d form. */
void la_alsl(struct code *c, int rd, int rj, int rk, int sa, int d);
/* revb.2h / revb.4h / revb.2w / revb.d: the bytes reversed within each
 * halfword, each halfword of 64 bits, each word, or the whole register. */
enum { LA_REVB_2H, LA_REVB_4H, LA_REVB_2W, LA_REVB_D };
void la_revb(struct code *c, int op, int rd, int rj);

/* ---- constants and addresses ------------------------------------------ */

void la_lu12i(struct code *c, int rd, long si20);    /* lu12i.w */
void la_lu32i(struct code *c, int rd, long si20);    /* lu32i.d */
void la_lu52i(struct code *c, int rd, int rj, long si12);   /* lu52i.d */
/* The PC-relative ones: pcaddi (pc + si20 << 2), pcalau12i (the 4 KiB
 * page of pc + si20 << 12), pcaddu12i (pc + si20 << 12) and pcaddu18i
 * (pc + si20 << 18). */
enum { LA_PCADDI, LA_PCALAU12I, LA_PCADDU12I, LA_PCADDU18I };
void la_pcrel(struct code *c, int op, int rd, long si20);

/* rd = v, as few instructions as the value needs, the sequence LLVM's
 * LoongArchMatInt chooses: one ori or addi.w for 12 bits, lu12i.w and an
 * ori for 32, then lu32i.d and lu52i.d for the upper halves only when
 * the sign extension of what is below does not already give them. */
void la_li(struct code *c, int rd, long long v);
int la_li_len(long long v);

/* ---- memory ---------------------------------------------------------- */

/* rd = *(rj + off), `size` 1/2/4/8, `sign` for a narrow load; the offset
 * a signed 12-bit field. */
void la_load(struct code *c, int rd, int rj, int off, int size, int sign);
void la_store(struct code *c, int rd, int rj, int off, int size);

/* ---- control flow ----------------------------------------------------- */

/* The conditional branches. beq..bgeu compare rj with rd (`blt rj, rd`
 * is taken when rj < rd); beqz/bnez test rj alone and reach further. */
enum { LA_BEQ, LA_BNE, LA_BLT, LA_BGE, LA_BLTU, LA_BGEU, LA_BEQZ, LA_BNEZ };
int la_branch_reaches(int cond, long off);
unsigned long la_enc_branch(int cond, int rj, int rd, long off);
/* A branch with its offset left zero, for la_patch_b; returns its
 * offset in the code. */
int la_b_placeholder(struct code *c, int cond, int rj, int rd);
/* The branch at `at` (any of the eight), retargeted at `target`; returns 0
 * and writes nothing when the distance does not fit its field. */
int la_patch_b(struct code *c, int at, int target);

/* b (link 0) or bl (link 1), with its offset left zero / patched. */
int la_j_placeholder(struct code *c, int link);
int la_patch_j(struct code *c, int at, int target);
unsigned long la_enc_j(int link, long off);

/* jirl rd, rj, off: rd = pc + 4, pc = rj + off (bytes, 18-bit signed,
 * a multiple of four). */
void la_jirl(struct code *c, int rd, int rj, long off);
void la_ret(struct code *c);                     /* jirl zero, ra, 0 */

/* ---- atomics and barriers --------------------------------------------- */

/* ll.w/ll.d rd, rj, off and sc.w/sc.d rd, rj, off: the offset in bytes,
 * a multiple of four in 16 signed bits. sc writes rd = 1 when the store
 * happened and 0 when the reservation was lost. */
void la_ll(struct code *c, int rd, int rj, int off, int d);
void la_sc(struct code *c, int rd, int rj, int off, int d);
/* amswap_db, amadd_db, amand_db, amor_db, amxor_db: rd = *rj, *rj = rd'
 * where rd' is rk, or *rj <op> rk -- with a full barrier (the _db forms).
 * rd may be neither rj nor rk (the instruction is undefined then, and
 * llvm-mc refuses it); that is checked. */
enum { LA_AMSWAP, LA_AMADD, LA_AMAND, LA_AMOR, LA_AMXOR };
void la_am(struct code *c, int op, int rd, int rk, int rj, int d);
void la_dbar(struct code *c, int hint);
void la_break(struct code *c, int code);

/* ---- what only the assembler emits -------------------------------------
 *
 * The instructions src/arch/loongarch/asm.c accepts that the code
 * generator never emits, by name: each with its FORMAT (which fields it
 * has) and its opcode word -- the instruction with every field zero, as
 * llvm-mc encodes it. The format packers above place the fields, so these
 * too are derived rather than restated, and tests/golden/loongarch-asm.sh
 * compares each with llvm-mc. */
enum {
    LAF_2R,         /* rd, rj */
    LAF_3R,         /* rd, rj, rk (indexed loads and stores, mulw) */
    LAF_AM,         /* rd, rk, rj: an AM* atomic; rd may be neither */
    LAF_PTR,        /* rd, rj, si14 << 2 (ldptr/stptr) */
    LAF_CSRRD,      /* rd, csr */
    LAF_CSRWR,      /* rd, csr */
    LAF_CSRXCHG,    /* rd, rj, csr (rj not r0 or r1) */
    LAF_CODE15,     /* a 15-bit code or hint (syscall, idle, ibar) */
    LAF_NONE,       /* no operand (ertn) */
    LAF_BSTRINS     /* rd, rj, msb, lsb, w or d by the opcode */
};
struct la_raw {
    const char *name;
    int fmt;
    unsigned long op;
    int d;          /* a 64-bit (.d) form: bit-field widths, ptr scale */
};
extern const struct la_raw la_raw_insns[];

#endif
