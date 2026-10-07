/* MIPS32 Release 2 instruction encoding, in either byte order (mipsel,
 * mips): an instruction is a 32-bit word stored in the target's order.
 *
 * Three formats carry almost everything -- R (register), I (16-bit
 * immediate) and J (26-bit jump target) -- and the rest are the same
 * fields under another opcode (SPECIAL2's mul and clz, SPECIAL3's
 * seb/seh/wsbh/ext/ins, REGIMM's bltz/bgez/bal, COP0's mfc0/mtc0). So
 * this file packs three formats once and every instruction is its
 * opcode, its function code and which register goes in which field: a
 * new instruction cannot put a field in the wrong place, because it
 * does not name the places (the RISC-V encoder's rule, which held).
 *
 * The immediates are where MIPS differs from its neighbours, and the
 * difference is the kind that encodes cleanly and computes something
 * else: addiu, slti, sltiu, the loads and the stores SIGN-extend their
 * 16 bits, and andi, ori and xori ZERO-extend theirs. sltiu
 * sign-extends and then compares unsigned. Each wrapper checks the range
 * of the extension its instruction performs and stops with an internal
 * error rather than truncate.
 *
 * Every transfer of control has a DELAY SLOT: the next instruction runs
 * before the branch takes effect. Nothing here emits the slot -- the
 * code generator does, and it emits a nop (docs/internals/mips32-plan.md).
 *
 * tools/mipscheck compares every form below with llvm-mc
 * (tests/golden/mips-encoding.sh).
 */
#ifndef EMBCC_ARCH_MIPS_EMIT_H
#define EMBCC_ARCH_MIPS_EMIT_H

#include "../code.h"

/* The register file, by o32 name. */
enum {
    MIPS_ZERO = 0, MIPS_AT = 1,
    MIPS_V0 = 2, MIPS_V1 = 3,
    MIPS_A0 = 4, MIPS_A1 = 5, MIPS_A2 = 6, MIPS_A3 = 7,
    MIPS_T0 = 8, MIPS_T1 = 9, MIPS_T2 = 10, MIPS_T3 = 11,
    MIPS_T4 = 12, MIPS_T5 = 13, MIPS_T6 = 14, MIPS_T7 = 15,
    MIPS_S0 = 16, MIPS_S1 = 17, MIPS_S2 = 18, MIPS_S3 = 19,
    MIPS_S4 = 20, MIPS_S5 = 21, MIPS_S6 = 22, MIPS_S7 = 23,
    MIPS_T8 = 24, MIPS_T9 = 25, MIPS_K0 = 26, MIPS_K1 = 27,
    MIPS_GP = 28, MIPS_SP = 29, MIPS_FP = 30, MIPS_RA = 31
};

/* The argument registers, in order. */
#define MIPS_NARGREG 4
extern const int mips_argreg[MIPS_NARGREG];

/* The o32 name of a register ("zero", "at", "v0", ... "ra"), for -S,
 * inline asm and diagnostics. */
const char *mips_reg_name(int r);

/* ---- the formats ---------------------------------------------------- */

/* R: op(6) rs(5) rt(5) rd(5) sa(5) funct(6). */
unsigned long mips_enc_r(int op, int rs, int rt, int rd, int sa, int funct);
/* I: op(6) rs(5) rt(5) imm(16). `imm` is the FIELD, 0..0xffff; the
 * wrappers below decide how a value becomes one. */
unsigned long mips_enc_i(int op, int rs, int rt, unsigned imm);
/* J: op(6) target(26), the target a WORD index. */
unsigned long mips_enc_j(int op, unsigned long target26);

/* Does `v` fit a sign-extended (`sign`) or zero-extended 16-bit field? */
int mips_fits16(long long v, int sign);

/* The byte order instruction words are stored in: little-endian until
 * told otherwise. The code generator sets it from target_big_endian(),
 * EmbLD from the objects it links. */
void mips_set_big_endian(int on);
int mips_big_endian(void);
/* A word at p, in that order. */
void mips_put_word(unsigned char *p, unsigned long w);
unsigned long mips_get_word(const unsigned char *p);
/* Every instruction becomes four bytes in that order here. */
void mips_w(struct code *c, unsigned long w);
/* ...and the word already emitted at `at`, read and rewritten. */
unsigned long mips_rdw(const struct code *c, int at);
void mips_wrw(struct code *c, int at, unsigned long w);

/* ---- moves and constants ---------------------------------------------- */

void mips_nop(struct code *c);                     /* sll $0, $0, 0 */
void mips_mv(struct code *c, int rd, int rs);      /* or rd, rs, $0 */
void mips_lui(struct code *c, int rt, unsigned imm16);
/* rd = v, a 32-bit value (the low 32 bits of `v` are what count): one
 * addiu or ori when the value fits either 16-bit extension, else lui and
 * an ori of the low half when it is not zero. */
void mips_li(struct code *c, int rd, long long v);
int mips_li_len(long long v);

/* ---- arithmetic and logic --------------------------------------------- */

/* rd = a OP b, in source order whatever the field order: for the
 * variable shifts `a` is the value and `b` the amount (sllv puts the
 * amount in rs), for movn/movz rd = a when b is nonzero / zero. */
enum mips_alu {
    MIPS_ADDU, MIPS_SUBU, MIPS_AND, MIPS_OR, MIPS_XOR, MIPS_NOR,
    MIPS_SLT, MIPS_SLTU, MIPS_SLLV, MIPS_SRLV, MIPS_SRAV, MIPS_ROTRV,
    MIPS_MOVN, MIPS_MOVZ, MIPS_MUL
};
void mips_alu(struct code *c, int op, int rd, int a, int b);

/* rt = rs OP imm. addiu/slti/sltiu take a SIGNED value, andi/ori/xori
 * an UNSIGNED one (0..65535); anything else is an internal error. */
enum mips_aluimm { MIPS_ADDIU, MIPS_SLTI, MIPS_SLTIU, MIPS_ANDI,
                   MIPS_ORI, MIPS_XORI };
void mips_alu_imm(struct code *c, int op, int rt, int rs, long long imm);
/* Whether alu_imm takes this value for this operation. */
int mips_alu_imm_ok(int op, long long imm);

/* rd = rt shifted by a constant 0..31. */
enum mips_shift { MIPS_SLL, MIPS_SRL, MIPS_SRA, MIPS_ROTR };
void mips_shift_imm(struct code *c, int op, int rd, int rt, int sa);

/* HI:LO = rs * rt, or LO = rs / rt and HI = rs % rt. Never traps. */
enum mips_muldiv { MIPS_MULT, MIPS_MULTU, MIPS_DIV, MIPS_DIVU };
void mips_muldiv(struct code *c, int op, int rs, int rt);
void mips_mfhi(struct code *c, int rd);
void mips_mflo(struct code *c, int rd);
void mips_mthi(struct code *c, int rs);
void mips_mtlo(struct code *c, int rs);

/* Release 2's bit manipulation. */
void mips_clz(struct code *c, int rd, int rs);
void mips_clo(struct code *c, int rd, int rs);
void mips_seb(struct code *c, int rd, int rt);
void mips_seh(struct code *c, int rd, int rt);
void mips_wsbh(struct code *c, int rd, int rt);
/* rt = the `size` bits of rs at `pos` (ext), or those bits of rt replaced
 * by the low `size` bits of rs (ins); 0 <= pos, 1 <= size, pos+size <= 32. */
void mips_ext(struct code *c, int rt, int rs, int pos, int size);
void mips_ins(struct code *c, int rt, int rs, int pos, int size);

/* ---- memory ----------------------------------------------------------- */

/* rt = *(base + off), 1, 2 or 4 bytes, `sign` choosing lb/lh over
 * lbu/lhu; the offset is a signed 16-bit field. */
void mips_load(struct code *c, int rt, int base, int off, int size, int sign);
void mips_store(struct code *c, int rt, int base, int off, int size);
/* The unaligned halves (Release 2 has them; Release 6 removed them).
 * Little-endian: a word at any address is `lwl rt, off+3(b)` then
 * `lwr rt, off(b)`; big-endian, `lwl rt, off(b)` then `lwr rt, off+3(b)`
 * (lwl always fills the register's most significant end, from the byte
 * the address names toward the word's boundary). Stored the same way
 * with swl/swr. rt must not be the base for the load pair -- the first
 * half writes it. */
void mips_lwl(struct code *c, int rt, int base, int off);
void mips_lwr(struct code *c, int rt, int base, int off);
void mips_swl(struct code *c, int rt, int base, int off);
void mips_swr(struct code *c, int rt, int base, int off);
/* The load-linked / store-conditional pair. sc writes 1 to rt on success
 * and 0 on failure. */
void mips_ll(struct code *c, int rt, int base, int off);
void mips_sc(struct code *c, int rt, int base, int off);
void mips_sync(struct code *c, int stype);

/* ---- control flow ----------------------------------------------------- */

/* The conditional branches. BEQ/BNE compare two registers; the other four
 * compare one with zero (rt is ignored). */
enum mips_cond { MIPS_BEQ, MIPS_BNE, MIPS_BLEZ, MIPS_BGTZ, MIPS_BLTZ,
                 MIPS_BGEZ, MIPS_BAL };
/* A branch whose displacement is `off` BYTES from the delay slot (the
 * instruction after the branch): a multiple of 4 in -131072..131068. */
unsigned long mips_enc_branch(int cond, int rs, int rt, long off);
/* Emitted with a zero displacement; returns its offset for mips_patch_b. */
int mips_b_placeholder(struct code *c, int cond, int rs, int rt);
/* Points the branch at `at` to `target` (offsets in the buffer). Returns
 * 0 when the target is out of reach, which is the caller's to refuse. */
int mips_patch_b(struct code *c, int at, int target);
/* Can a branch at `at` reach `target`? */
int mips_b_reaches(long at, long target);

/* jal/j with a zero target, for an R_MIPS_26 relocation. */
void mips_jal(struct code *c);
void mips_j(struct code *c);
void mips_jr(struct code *c, int rs);
void mips_jalr(struct code *c, int rd, int rs);

/* ---- traps and the system --------------------------------------------- */

void mips_break(struct code *c, int code);         /* code 0..1023 */
void mips_syscall(struct code *c);
void mips_teq(struct code *c, int rs, int rt, int code);
void mips_eret(struct code *c);
void mips_wait(struct code *c);
void mips_ehb(struct code *c);
void mips_di(struct code *c, int rt);
void mips_ei(struct code *c, int rt);
void mips_mfc0(struct code *c, int rt, int rd, int sel);
void mips_mtc0(struct code *c, int rt, int rd, int sel);

#endif
