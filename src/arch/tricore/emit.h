/* Infineon TriCore 1.6 instruction encoding (AURIX TC2xx/TC3xx),
 * little-endian. docs/internals/tricore-plan.md.
 *
 * TriCore has TWO register files of sixteen: data registers D0-D15 and
 * address registers A0-A15. An instruction names which file each field is
 * from by its opcode, never by the number, so every emitter below says in
 * its parameter names which file a register comes from (dc, da, ab, ...):
 * `d` a data register, `a` an address register, `e` an even data register
 * that names the pair E[n] = D[n+1]:D[n]. Numbers are 0-15 in both files.
 *
 * Instructions are 32 or 16 bits; bit 0 of the first halfword says which
 * (1: 32-bit). The low byte of a 32-bit word is the primary opcode, op1,
 * and most formats have a secondary op2 somewhere else. This file packs
 * each FORMAT once (tc_enc_*) and every instruction is its op1/op2 and
 * which operand goes in which field -- so a new instruction cannot put a
 * field in the wrong place, because it does not name the places. Only the
 * 32-bit forms are emitted by the code generator today, and the 16-bit
 * RET and NOP.
 *
 * The immediates differ by instruction, and a mistake there encodes
 * cleanly and computes something else: RC-format const9 is SIGN-extended
 * for ADD, EQ, LT, GE, MIN, MAX, RSUB and MUL and ZERO-extended for the
 * logic operations and the unsigned compares; RLC's const16 is
 * sign-extended by MOV and ADDI and zero-extended by MOV.U; MOVH and
 * ADDIH put it in the high half. Each wrapper checks the range its
 * instruction's extension can represent and stops with an internal error
 * rather than truncate.
 *
 * There is no disassembler for TriCore in LLVM or QEMU on this machine.
 * The referee is QEMU's TRANSLATOR: tools/tricorecheck builds an image of
 * every form, QEMU translates it one instruction per block with -d op,
 * and each instruction's TCG operations -- `add_i32 loc3,d4,d5`, a load
 * from `a12` plus `$0xffff8010` -- are compared with what the operands
 * say it must do (tests/golden/tricore-encoding.sh).
 */
#ifndef EMBCC_ARCH_TRICORE_EMIT_H
#define EMBCC_ARCH_TRICORE_EMIT_H

#include "../code.h"

/* The address registers with a fixed role in the EABI. A0, A1, A8 and A9
 * are the system's global address registers and never touched. */
enum { TC_SP = 10, TC_RA = 11 };

/* The core special function registers the code and the harness reach with
 * MTCR/MFCR (their CSFR offsets). */
enum {
    TC_CSFR_PCXI = 0xfe00, TC_CSFR_PSW = 0xfe04, TC_CSFR_SYSCON = 0xfe14,
    TC_CSFR_BIV = 0xfe20, TC_CSFR_BTV = 0xfe24, TC_CSFR_ISP = 0xfe28,
    TC_CSFR_FCX = 0xfe38, TC_CSFR_LCX = 0xfe3c
};

/* "d4", "a10", "e2": a register's name in its file ('d', 'a' or 'e'). */
const char *tc_reg_name(int file, int r);

/* ---- the formats --------------------------------------------------------
 *
 * Each takes FIELD values (already reduced to their width) and places
 * them; the wrappers below decide how a value becomes a field. */
unsigned long tc_enc_rr(int op1, int op2, int d, int s1, int s2, int n);
unsigned long tc_enc_rr2(int op1, int op2, int d, int s1, int s2);
unsigned long tc_enc_rc(int op1, int op2, int d, int s1, unsigned c9);
unsigned long tc_enc_rlc(int op1, int d, int s1, unsigned c16);
unsigned long tc_enc_rrr(int op1, int op2, int d, int s1, int s2, int s3,
                         int n);
unsigned long tc_enc_rrr2(int op1, int op2, int d, int s1, int s2, int s3);
unsigned long tc_enc_rrrr(int op1, int op2, int d, int s1, int s2, int s3);
unsigned long tc_enc_rrpw(int op1, int op2, int d, int s1, int s2, int pos,
                          int width);
unsigned long tc_enc_rcpw(int op1, int op2, int d, int s1, unsigned c4,
                          int pos, int width);
unsigned long tc_enc_bol(int op1, int s1d, int s2, unsigned off16);
unsigned long tc_enc_brr(int op1, int op2, int s1, int s2, unsigned disp15);
unsigned long tc_enc_brc(int op1, int op2, int s1, unsigned c4,
                         unsigned disp15);
unsigned long tc_enc_b(int op1, unsigned long disp24);
unsigned long tc_enc_sys(int op1, int op2, int s1d);

/* A 32-bit instruction as four little-endian bytes, a 16-bit one as two. */
void tc_w(struct code *c, unsigned long w);
void tc_h(struct code *c, unsigned h);

/* Does `v` fit a sign-extended (`sign`) or zero-extended field of `bits`? */
int tc_fits(long long v, int bits, int sign);

/* ---- moves and constants ---------------------------------------------- */

void tc_mov(struct code *c, int dc, int db);          /* D[c] = D[b] */
void tc_mov_a(struct code *c, int ac, int db);        /* A[c] = D[b] */
void tc_mov_d(struct code *c, int dc, int ab);        /* D[c] = A[b] */
void tc_mov_aa(struct code *c, int ac, int ab);       /* A[c] = A[b] */
/* D[c] = a constant: MOV sign-extends 16 bits, MOV.U zero-extends them,
 * MOVH puts them in the high half. */
void tc_mov_imm(struct code *c, int dc, long long v);
void tc_mov_u(struct code *c, int dc, long long v);
void tc_movh(struct code *c, int dc, unsigned v16);
void tc_movh_a(struct code *c, int ac, unsigned v16);
/* D[c] = v, a 32-bit value (its low 32 bits): one MOV, MOV.U or MOVH
 * when one does, else MOVH and an ADDI of the sign-extended low half. */
void tc_li(struct code *c, int dc, long long v);
int tc_li_len(long long v);
/* The high and low halves of an address for MOVH(.A)/ADDIH and a
 * sign-extended 16-bit low part (ADDI, LEA, a load's offset): hi is
 * rounded so that (hi << 16) + (short)lo is v. */
unsigned tc_hi_adj(unsigned long v);
/* A[c] = v: MOVH.A, then LEA of the low half when it is not zero. */
void tc_li_a(struct code *c, int ac, unsigned long v);

void tc_addi(struct code *c, int dc, int da, long long v);    /* sext16 */
void tc_addih(struct code *c, int dc, int da, unsigned v16);   /* << 16 */
void tc_addih_a(struct code *c, int ac, int aa, unsigned v16);
void tc_lea(struct code *c, int ac, int ab, long long off);    /* sext16 */
void tc_add_a(struct code *c, int ac, int aa, int ab);
void tc_sub_a(struct code *c, int ac, int aa, int ab);
/* A[c] = A[b] + (D[a] << n), n 0..3. */
void tc_addsc_a(struct code *c, int ac, int ab, int da, int n);

/* ---- arithmetic and logic --------------------------------------------- */

/* D[c] = D[a] OP D[b] (or OP const9 for tc_alu_imm). The comparisons
 * leave 0 or 1. SH and SHA shift LEFT by a positive count and RIGHT by a
 * negative one (the count's low six bits, -32..31); SHA's right shift is
 * arithmetic. ADDX/SUBX set the carry PSW.C, ADDC/SUBC add it in (SUBC:
 * a + ~b + C), which is the 64-bit add and subtract. RSUB is immediate
 * only: D[c] = const9 - D[a]. MUL is the low 32 bits. */
enum tc_alu {
    TC_ADD, TC_SUB, TC_ADDX, TC_ADDC, TC_SUBX, TC_SUBC,
    TC_EQ, TC_NE, TC_LT, TC_LTU, TC_GE, TC_GEU,
    TC_MIN, TC_MINU, TC_MAX, TC_MAXU,
    TC_AND, TC_OR, TC_XOR, TC_NOR, TC_ANDN, TC_ORN, TC_NAND, TC_XNOR,
    TC_SH, TC_SHA, TC_MUL, TC_RSUB,
    TC_NALU
};
void tc_alu(struct code *c, int op, int dc, int da, int db);
/* Whether op has an immediate form that represents `imm`, and emit it. */
int tc_alu_imm_ok(int op, long long imm);
void tc_alu_imm(struct code *c, int op, int dc, int da, long long imm);

/* E[c] = D[a] * D[b], all 64 bits, signed or unsigned. */
void tc_mul64(struct code *c, int ec, int da, int db, int sign);
/* D[c] = D[d] + D[a] * D[b], the low 32 bits. */
void tc_madd(struct code *c, int dc, int dd, int da, int db);
/* E[c] = D[a] / D[b]: the quotient in D[c], the remainder in D[c+1],
 * truncating toward zero (TriCore 1.6's DIV and DIV.U). */
void tc_div(struct code *c, int ec, int da, int db, int sign);
void tc_clz(struct code *c, int dc, int da);

/* D[c] = the `width` bits of D[a] at `pos`, sign- or zero-extended. */
void tc_extr(struct code *c, int dc, int da, int pos, int width, int sign);
/* D[c] = D[a] with the low `width` bits of D[b] (or of a 4-bit constant)
 * put at `pos`. */
void tc_insert(struct code *c, int dc, int da, int db, int pos, int width);
void tc_insert_imm(struct code *c, int dc, int da, unsigned k4, int pos,
                   int width);
/* D[c] = the high word of ({D[hi], D[lo]} << pos), pos 0..31 -- a funnel
 * shift -- with pos a constant or the low five bits of D[p]. */
void tc_dextr(struct code *c, int dc, int dhi, int dlo, int pos);
void tc_dextr_r(struct code *c, int dc, int dhi, int dlo, int dp);
/* D[c] = D[cond] != 0 ? D[t] : D[f] (SEL); tc_seln tests == 0. */
void tc_sel(struct code *c, int dc, int dcond, int dt, int df);
void tc_seln(struct code *c, int dc, int dcond, int dt, int df);

/* ---- memory ----------------------------------------------------------- */

/* D[t] = *(A[b] + off), 1, 2 or 4 bytes, `sign` choosing LD.B/LD.H over
 * LD.BU/LD.HU; the offset is the long-offset (BOL) form's signed 16 bits.
 * The stores truncate. */
void tc_load(struct code *c, int dt, int ab, long long off, int size,
             int sign);
void tc_store(struct code *c, int dt, int ab, long long off, int size);
void tc_ld_a(struct code *c, int at, int ab, long long off);
void tc_st_a(struct code *c, int at, int ab, long long off);

/* ---- control flow ----------------------------------------------------- */

/* The conditional branches, each comparing two registers (BRR) or a
 * register with a 4-bit constant (BRC). JEQ_A and JNE_A compare two
 * ADDRESS registers and have no constant form; JZ_A and JNZ_A test one
 * address register against zero. The BRC constant is sign-extended for
 * JEQ, JNE, JLT and JGE and zero-extended for JLTU and JGEU. The
 * displacement is in BYTES from the branch itself, even, -32768..32766. */
enum tc_cond {
    TC_JEQ, TC_JNE, TC_JLT, TC_JLTU, TC_JGE, TC_JGEU,
    TC_JEQ_A, TC_JNE_A, TC_JZ_A, TC_JNZ_A
};
unsigned long tc_enc_jcc(int cond, int s1, int s2, long off);
unsigned long tc_enc_jcci(int cond, int s1, long long k4, long off);
int tc_jcci_ok(int cond, long long k4);
/* Emitted with a zero displacement; return their offset for tc_patch. */
int tc_jcc_placeholder(struct code *c, int cond, int s1, int s2);
int tc_jcci_placeholder(struct code *c, int cond, int s1, long long k4);
/* J (24-bit displacement, +-16 MiB): to a label, or with a relocation. */
unsigned long tc_enc_j(long off);
int tc_j_placeholder(struct code *c);
/* Points the branch or jump at `at` to `target` (offsets in the buffer),
 * whichever format it is. Returns 0 when the target is out of reach --
 * the caller's to handle -- and never truncates. */
int tc_patch(struct code *c, int at, int target);
int tc_br_reaches(long at, long target);

/* CALL and J with a zero displacement, for an R_TRICORE_24REL. */
void tc_call0(struct code *c);
void tc_j0(struct code *c);
unsigned long tc_enc_call(long off);
/* JL: jump and link (A11 = the return address), no context saved. */
unsigned long tc_enc_jl(long off);
void tc_ji(struct code *c, int aa);
void tc_jli(struct code *c, int aa);
void tc_calli(struct code *c, int aa);
void tc_ret(struct code *c);          /* RET, the 32-bit form */
void tc_ret16(struct code *c);        /* RET, the 16-bit form */

/* ---- the system ------------------------------------------------------- */

void tc_nop(struct code *c);
void tc_nop16(struct code *c);
void tc_debug(struct code *c);
void tc_isync(struct code *c);
void tc_dsync(struct code *c);
void tc_svlcx(struct code *c);
void tc_rslcx(struct code *c);
void tc_enable(struct code *c);
void tc_disable(struct code *c);
void tc_rfe(struct code *c);
/* SYSCALL const9: a system-call trap (class 6) with the constant as its
 * identification number. */
void tc_syscall(struct code *c, unsigned k9);
void tc_mtcr(struct code *c, unsigned csfr, int da);
void tc_mfcr(struct code *c, int dc, unsigned csfr);

#endif
