/* 32-bit PowerPC instruction encoding (powerpc-none-eabi): every
 * instruction is one 32-bit word, stored BIG-endian, the most significant
 * byte at the lowest address -- the only order this target has.
 *
 * Six formats carry everything the backend emits: D (a register, a base
 * or source register and a 16-bit immediate), X and XO (three registers
 * and an extended opcode, the XO ones with an overflow bit left 0), M
 * (rlwinm's rotate and mask), I (b/bl's 24-bit displacement) and B
 * (bc's 14-bit one), and XL (blr, bctr, the CR logic). So this file packs
 * the formats once and every instruction is its opcode, its extended
 * opcode and which register goes in which field (the MIPS and RISC-V
 * encoders' rule): a new instruction cannot put a field in the wrong
 * place, because it does not name the places.
 *
 * The traps this machine sets are fields that encode cleanly and mean
 * something else:
 *   - r0 as the base of a load or store, or as the source of addi/addis,
 *     reads as the NUMBER 0, not as r0 ("li" is addi rd, 0, imm). The
 *     wrappers refuse r0 there, except where the caller asks for li/lis.
 *   - the logical immediates (ori, xori, andi.) ZERO-extend their 16
 *     bits, addi, cmpwi and mulli SIGN-extend them, and cmplwi compares
 *     a zero-extended one.
 *   - the logical X-forms put the SOURCE in the first field (rs) and the
 *     destination in the second (ra); the arithmetic ones the reverse.
 *   - subf subtracts its first operand FROM its second.
 * Each wrapper below takes its operands in source order (d = a OP b) and
 * checks every range, stopping with an internal error rather than
 * truncating.
 *
 * tools/ppccheck compares every form below with llvm-mc
 * (tests/golden/ppc-encoding.sh).
 */
#ifndef EMBCC_ARCH_PPC_EMIT_H
#define EMBCC_ARCH_PPC_EMIT_H

#include "../code.h"

/* The register file: r0 (scratch, and "0" as a base), r1 the stack
 * pointer, r2 and r13 the EABI small-data anchors (never touched), r3-r10
 * arguments, r3:r4 results, r11-r12 scratch, r14-r31 callee-saved. */
enum { PPC_R0 = 0, PPC_SP = 1, PPC_R2 = 2, PPC_R3 = 3, PPC_R4 = 4,
       PPC_R5 = 5, PPC_R6 = 6, PPC_R7 = 7, PPC_R8 = 8, PPC_R9 = 9,
       PPC_R10 = 10, PPC_R11 = 11, PPC_R12 = 12, PPC_R13 = 13,
       PPC_R14 = 14, PPC_R31 = 31 };

#define PPC_NARGREG 8          /* r3..r10 */

/* The word at p, and a word to p: big-endian. */
void ppc_put_word(unsigned char *p, unsigned long w);
unsigned long ppc_get_word(const unsigned char *p);
void ppc_w(struct code *c, unsigned long w);
unsigned long ppc_rdw(const struct code *c, int at);
void ppc_wrw(struct code *c, int at, unsigned long w);

/* ---- the opcodes -----------------------------------------------------
 * The primary opcodes and opcode 31's and 19's extended opcodes, as the
 * Power ISA (Book I and Book III-E) names them. The code generator uses
 * a few through the wrappers below; the assembler (asm.c) names each one
 * it encodes from here. An XO-form's OE bit is PPC_XO_OE in the extended
 * opcode. */
enum {
    PPC_OP_TWI = 3, PPC_OP_MULLI = 7, PPC_OP_SUBFIC = 8, PPC_OP_CMPLI = 10,
    PPC_OP_CMPI = 11, PPC_OP_ADDIC = 12, PPC_OP_ADDIC_ = 13,
    PPC_OP_ADDI = 14, PPC_OP_ADDIS = 15, PPC_OP_BC = 16, PPC_OP_SC = 17,
    PPC_OP_B = 18, PPC_OP_19 = 19, PPC_OP_RLWIMI = 20, PPC_OP_RLWINM = 21,
    PPC_OP_RLWNM = 23, PPC_OP_ORI = 24, PPC_OP_ORIS = 25, PPC_OP_XORI = 26,
    PPC_OP_XORIS = 27, PPC_OP_ANDI = 28, PPC_OP_ANDIS = 29, PPC_OP_31 = 31,
    PPC_OP_LWZ = 32, PPC_OP_LWZU = 33, PPC_OP_LBZ = 34, PPC_OP_LBZU = 35,
    PPC_OP_STW = 36, PPC_OP_STWU = 37, PPC_OP_STB = 38, PPC_OP_STBU = 39,
    PPC_OP_LHZ = 40, PPC_OP_LHZU = 41, PPC_OP_LHA = 42, PPC_OP_LHAU = 43,
    PPC_OP_STH = 44, PPC_OP_STHU = 45, PPC_OP_LMW = 46, PPC_OP_STMW = 47
};
enum {                                      /* opcode 31's, X and XO */
    PPC_X_CMP = 0, PPC_X_TW = 4, PPC_X_SUBFC = 8, PPC_X_ADDC = 10,
    PPC_X_MULHWU = 11, PPC_X_ISEL = 15, PPC_X_MFCR = 19, PPC_X_LWARX = 20,
    PPC_X_LWZX = 23, PPC_X_SLW = 24, PPC_X_CNTLZW = 26, PPC_X_AND = 28,
    PPC_X_CMPL = 32, PPC_X_SUBF = 40, PPC_X_DCBST = 54, PPC_X_LWZUX = 55,
    PPC_X_ANDC = 60, PPC_X_MULHW = 75, PPC_X_MFMSR = 83, PPC_X_DCBF = 86,
    PPC_X_LBZX = 87, PPC_X_NEG = 104, PPC_X_LBZUX = 119, PPC_X_NOR = 124,
    PPC_X_WRTEE = 131, PPC_X_SUBFE = 136, PPC_X_ADDE = 138,
    PPC_X_MTCRF = 144, PPC_X_MTMSR = 146, PPC_X_STWCX = 150,
    PPC_X_STWX = 151, PPC_X_WRTEEI = 163, PPC_X_STWUX = 183,
    PPC_X_SUBFZE = 200, PPC_X_ADDZE = 202, PPC_X_STBX = 215,
    PPC_X_SUBFME = 232, PPC_X_ADDME = 234, PPC_X_MULLW = 235,
    PPC_X_DCBTST = 246, PPC_X_STBUX = 247, PPC_X_ADD = 266,
    PPC_X_DCBT = 278, PPC_X_LHZX = 279, PPC_X_EQV = 284,
    PPC_X_LHZUX = 311, PPC_X_XOR = 316, PPC_X_MFSPR = 339,
    PPC_X_LHAX = 343, PPC_X_LHAUX = 375, PPC_X_STHX = 407,
    PPC_X_ORC = 412, PPC_X_STHUX = 439, PPC_X_OR = 444, PPC_X_DIVWU = 459,
    PPC_X_MTSPR = 467, PPC_X_DCBI = 470, PPC_X_NAND = 476,
    PPC_X_DIVW = 491, PPC_X_LWBRX = 534, PPC_X_SRW = 536,
    PPC_X_TLBSYNC = 566, PPC_X_SYNC = 598, PPC_X_STWBRX = 662,
    PPC_X_TLBIVAX = 786, PPC_X_LHBRX = 790, PPC_X_SRAW = 792,
    PPC_X_SRAWI = 824, PPC_X_MBAR = 854, PPC_X_TLBSX = 914,
    PPC_X_STHBRX = 918, PPC_X_EXTSH = 922, PPC_X_TLBRE = 946,
    PPC_X_EXTSB = 954, PPC_X_TLBWE = 978, PPC_X_ICBI = 982,
    PPC_X_DCBZ = 1014,
    PPC_XO_OE = 512
};
enum {                                      /* opcode 19's, XL */
    PPC_XL_MCRF = 0, PPC_XL_BCLR = 16, PPC_XL_CRNOR = 33,
    PPC_XL_RFMCI = 38, PPC_XL_RFI = 50, PPC_XL_RFCI = 51,
    PPC_XL_CRANDC = 129, PPC_XL_ISYNC = 150, PPC_XL_CRXOR = 193,
    PPC_XL_CRNAND = 225, PPC_XL_CRAND = 257, PPC_XL_CREQV = 289,
    PPC_XL_CRORC = 417, PPC_XL_CROR = 449, PPC_XL_BCCTR = 528
};

/* ---- the formats --------------------------------------------------- */
unsigned long ppc_enc_d(int op, int rt, int ra, unsigned imm16);
unsigned long ppc_enc_x(int op, int rt, int ra, int rb, int xo, int rc);
unsigned long ppc_enc_m(int op, int rs, int ra, int sh, int mb, int me,
                        int rc);
/* B-form: bc's BO and BI, a 16-bit displacement `bd` (a multiple of 4),
 * and the AA (absolute) and LK (link) bits. I-form: b's 26-bit `li`. */
unsigned long ppc_enc_bform(int bo, int bi, long bd, int aa, int lk);
unsigned long ppc_enc_iform(long li, int aa, int lk);

/* Does v fit a sign-extended (sign) or zero-extended 16-bit field? */
int ppc_fits16(long long v, int sign);

/* ---- constants and moves ------------------------------------------- */
void ppc_nop(struct code *c);                       /* ori 0, 0, 0 */
void ppc_mr(struct code *c, int rd, int rs);        /* or rd, rs, rs */
/* rd = v (its low 32 bits): li, lis, or lis and ori. */
void ppc_li(struct code *c, int rd, long long v);
int ppc_li_len(long long v);
void ppc_lis(struct code *c, int rd, unsigned imm16);   /* addis rd, 0, imm */

/* ---- immediates: d = s OP imm ---------------------------------------
 * ADDI, ADDIS, ADDIC, MULLI and SUBFIC (d = imm - s) take a SIGNED value;
 * ORI, ORIS, XORI, XORIS, ANDI_ (andi.) and ANDIS_ (andis.) an UNSIGNED
 * one, 0..65535. addi and addis refuse s == r0 (use ppc_li). */
enum ppc_immop { PPC_ADDI, PPC_ADDIS, PPC_ADDIC, PPC_MULLI, PPC_SUBFIC,
                 PPC_ORI, PPC_ORIS, PPC_XORI, PPC_XORIS, PPC_ANDI_,
                 PPC_ANDIS_ };
void ppc_imm(struct code *c, int op, int d, int s, long long imm);
int ppc_imm_ok(int op, long long imm);

/* ---- register operations: d = a OP b, in source order ---------------
 * SUB is a - b (subf d, b, a). The carrying forms: ADDC sets CA, ADDE adds
 * it; SUBC is a - b setting CA to "no borrow" and SUBE a - b - borrow
 * (subfc/subfe with the operands turned round). The shifts read six bits
 * of b: an amount of 32..63 shifts everything out (sraw: fills with the
 * sign). ANDC is a & ~b, ORC a | ~b. */
enum ppc_alu {
    PPC_ADD, PPC_SUB, PPC_ADDC, PPC_ADDE, PPC_SUBC, PPC_SUBE,
    PPC_MULLW, PPC_MULHW, PPC_MULHWU, PPC_DIVW, PPC_DIVWU,
    PPC_AND, PPC_ANDC, PPC_OR, PPC_ORC, PPC_XOR, PPC_NAND, PPC_NOR,
    PPC_EQV, PPC_SLW, PPC_SRW, PPC_SRAW
};
void ppc_alu(struct code *c, int op, int d, int a, int b);

/* d = OP s: NEG (-s), ADDZE (s + CA), SUBFZE (~s + CA, so -s - borrow),
 * ADDME (s + CA - 1), CNTLZW, EXTSB, EXTSH. */
enum ppc_unop { PPC_NEG, PPC_ADDZE, PPC_SUBFZE, PPC_ADDME, PPC_SUBFME,
                PPC_CNTLZW, PPC_EXTSB, PPC_EXTSH };
void ppc_un(struct code *c, int op, int d, int s);

/* srawi d, s, sh (0..31); sets CA. */
void ppc_srawi(struct code *c, int d, int s, int sh);
/* rlwinm d, s, sh, mb, me: s rotated left by sh, ANDed with the mask of
 * bits mb..me (bit 0 the most significant; mb > me wraps round). */
void ppc_rlwinm(struct code *c, int d, int s, int sh, int mb, int me);
/* rlwimi: the masked bits of the rotated s inserted into d. */
void ppc_rlwimi(struct code *c, int d, int s, int sh, int mb, int me);
/* rlwnm: rotated left by the low five bits of rb. */
void ppc_rlwnm(struct code *c, int d, int s, int rb, int mb, int me);
void ppc_slwi(struct code *c, int d, int s, int n);    /* n 0..31 */
void ppc_srwi(struct code *c, int d, int s, int n);    /* n 0..31 */

/* ---- comparisons, into CR field `cr` (0..7) ------------------------- */
/* cmpw (sign) / cmplw a, b */
void ppc_cmp(struct code *c, int cr, int sign, int a, int b);
/* cmpwi a, SIGNED imm (sign) / cmplwi a, UNSIGNED imm */
void ppc_cmpi(struct code *c, int cr, int sign, int a, long long imm);
/* mfcr d: the whole condition register */
void ppc_mfcr(struct code *c, int d);
/* crxor bt, ba, bb (crxor 6,6,6 clears cr1.eq before a variadic call) */
void ppc_crxor(struct code *c, int bt, int ba, int bb);

/* ---- memory ----------------------------------------------------------
 * 1, 2 or 4 bytes at base + off (a signed 16-bit field); `sign` picks lha
 * over lhz. There is no sign-extending byte load (lbz then extsb), so
 * sign with size 1 is the caller's to extend. base may not be r0. */
void ppc_load(struct code *c, int rt, int base, int off, int size, int sign);
void ppc_store(struct code *c, int rs, int base, int off, int size);
/* ...at ra + rb (ra may not be r0) */
void ppc_loadx(struct code *c, int rt, int ra, int rb, int size, int sign);
void ppc_storex(struct code *c, int rs, int ra, int rb, int size);
/* stwu rs, off(ra): store and update ra (the frame's allocation) */
void ppc_stwu(struct code *c, int rs, int ra, int off);
void ppc_stwux(struct code *c, int rs, int ra, int rb);
/* The byte-reversed forms (a 2- or 4-byte value stored the other way). */
void ppc_lbrx(struct code *c, int rt, int ra, int rb, int size);
void ppc_stbrx(struct code *c, int rs, int ra, int rb, int size);
/* lwarx / stwcx. at (ra|0) + rb: the reservation pair; stwcx. sets cr0.eq
 * on success. */
void ppc_lwarx(struct code *c, int rt, int ra, int rb);
void ppc_stwcx(struct code *c, int rs, int ra, int rb);
void ppc_sync(struct code *c);              /* sync (msync on e500) */
void ppc_isync(struct code *c);

/* ---- control flow ----------------------------------------------------
 * A conditional branch tests one bit of a CR field: */
enum ppc_cond { PPC_LT, PPC_GE, PPC_GT, PPC_LE, PPC_EQ, PPC_NE };
int ppc_cond_invert(int cond);
/* bc to `off` bytes from itself: a multiple of 4 in -32768..32764. */
unsigned long ppc_enc_bc(int cond, int cr, long off);
/* bdnz: decrement CTR, branch while it is not zero (a counted loop). */
unsigned long ppc_enc_bdnz(long off);
/* b to `off` bytes from itself: a multiple of 4 within +-32 MiB. */
unsigned long ppc_enc_b(long off, int link);
/* Emitted with a zero displacement; patched by ppc_patch_branch, which
 * reads which of the two the word is. Returns 0 when the target is out
 * of reach. */
int ppc_bc_placeholder(struct code *c, int cond, int cr);
int ppc_b_placeholder(struct code *c);
int ppc_patch_branch(struct code *c, int at, int target);
int ppc_bc_reaches(long at, long target);
void ppc_bl(struct code *c);            /* bl 0, for an R_PPC_REL24 */
void ppc_b_rel(struct code *c);         /* b 0, likewise (a tail call) */
void ppc_blr(struct code *c);
void ppc_bctr(struct code *c);
void ppc_bctrl(struct code *c);
void ppc_mflr(struct code *c, int d);
void ppc_mtlr(struct code *c, int s);
void ppc_mtctr(struct code *c, int s);
void ppc_mfspr(struct code *c, int d, int spr);
void ppc_mtspr(struct code *c, int spr, int s);
void ppc_trap(struct code *c);           /* tw 31, 0, 0 */
void ppc_sc(struct code *c);
void ppc_tlbwe(struct code *c);
void ppc_mfmsr(struct code *c, int d);
void ppc_mtmsr(struct code *c, int s);
void ppc_wrteei(struct code *c, int e);

#endif
