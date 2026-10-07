/* SPARC V8 instruction encoding (the LEON3's integer unit, with its
 * MUL/DIV and CASA): an instruction is a 32-bit word, always stored
 * big-endian -- SPARC has no other byte order.
 *
 * Three formats carry everything. Format 1 is `call` (op 01, a 30-bit
 * word displacement). Format 2 (op 00) is `sethi` and the branches: a
 * 22-bit field under op2. Format 3 (op 10, the arithmetic, and op 11,
 * memory) is rd, op3, rs1 and either rs2 (i = 0) or a SIGNED 13-bit
 * immediate (i = 1). Every instruction below is one of these with its
 * op3 named, so a new instruction cannot put a field in the wrong place
 * (the rule the RISC-V, MIPS and PowerPC encoders follow).
 *
 * The register numbers are the hardware's: %g0-%g7 0-7, %o0-%o7 8-15,
 * %l0-%l7 16-23, %i0-%i7 24-31. %g0 reads as zero and discards writes.
 *
 * Every transfer of control has a DELAY SLOT, as on MIPS, and a branch
 * may ANNUL it (the `,a` forms). Nothing here emits the slot: the code
 * generator does. A branch's displacement is counted from the branch
 * ITSELF, not from its slot (MIPS's is from the slot).
 *
 * tools/sparccheck compares every form below with llvm-mc
 * (tests/golden/sparc-encoding.sh).
 */
#ifndef EMBCC_ARCH_SPARC_EMIT_H
#define EMBCC_ARCH_SPARC_EMIT_H

#include "../code.h"

enum {
    SP_G0 = 0, SP_G1, SP_G2, SP_G3, SP_G4, SP_G5, SP_G6, SP_G7,
    SP_O0 = 8, SP_O1, SP_O2, SP_O3, SP_O4, SP_O5, SP_O6, SP_O7,
    SP_L0 = 16, SP_L1, SP_L2, SP_L3, SP_L4, SP_L5, SP_L6, SP_L7,
    SP_I0 = 24, SP_I1, SP_I2, SP_I3, SP_I4, SP_I5, SP_I6, SP_I7
};
#define SP_SP SP_O6        /* the stack pointer */
#define SP_FP SP_I6        /* the frame pointer: the caller's %sp */

/* "g0" .. "i7", with %sp and %fp under their own names. */
const char *sparc_reg_name(int r);

/* ---- the formats ---------------------------------------------------- */

/* Format 3, register form: op(2) rd(5) op3(6) rs1(5) i=0 asi(8) rs2(5). */
unsigned long sparc_enc_rr(int op, int rd, int op3, int rs1, int rs2);
/* Format 3, immediate form: ... rs1(5) i=1 simm13. */
unsigned long sparc_enc_ri(int op, int rd, int op3, int rs1, long long simm);
/* Format 2: op=00 rd/cond(5) op2(3) imm22. */
unsigned long sparc_enc_f2(int rd, int op2, unsigned long imm22);

int sparc_simm13_ok(long long v);

/* A word in memory order (big-endian), and the code buffer's. */
void sparc_put_word(unsigned char *p, unsigned long w);
unsigned long sparc_get_word(const unsigned char *p);
void sparc_w(struct code *c, unsigned long w);
unsigned long sparc_rdw(const struct code *c, int at);
void sparc_wrw(struct code *c, int at, unsigned long w);

/* ---- arithmetic and logic --------------------------------------------
 *
 * The operations are named by their op3 (op = 10). The `cc` forms set the
 * integer condition codes; the shifts take only the low five bits of
 * their count. */
enum sparc_op3 {
    SP_ADD = 0x00, SP_AND = 0x01, SP_OR = 0x02, SP_XOR = 0x03,
    SP_SUB = 0x04, SP_ANDN = 0x05, SP_ORN = 0x06, SP_XNOR = 0x07,
    SP_ADDX = 0x08, SP_UMUL = 0x0a, SP_SMUL = 0x0b, SP_SUBX = 0x0c,
    SP_UDIV = 0x0e, SP_SDIV = 0x0f,
    SP_ADDCC = 0x10, SP_ANDCC = 0x11, SP_ORCC = 0x12, SP_XORCC = 0x13,
    SP_SUBCC = 0x14, SP_ANDNCC = 0x15, SP_ORNCC = 0x16, SP_XNORCC = 0x17,
    SP_ADDXCC = 0x18, SP_UMULCC = 0x1a, SP_SMULCC = 0x1b, SP_SUBXCC = 0x1c,
    SP_UDIVCC = 0x1e, SP_SDIVCC = 0x1f,
    SP_SLL = 0x25, SP_SRL = 0x26, SP_SRA = 0x27
};
/* rd = rs1 OP rs2 */
void sparc_alu(struct code *c, int op3, int rd, int rs1, int rs2);
/* rd = rs1 OP simm13 (a shift: a count 0..31) */
void sparc_alu_imm(struct code *c, int op3, int rd, int rs1, long long imm);

/* ---- moves and constants --------------------------------------------- */

void sparc_nop(struct code *c);                  /* sethi 0, %g0 */
void sparc_mov(struct code *c, int rd, int rs);  /* or %g0, rs, rd */
/* rd = imm22 << 10 */
void sparc_sethi(struct code *c, int rd, unsigned long imm22);
/* rd = v, a 32-bit value (its low 32 bits): one `or %g0, simm13` when it
 * fits, else sethi and an `or` of the low ten bits when they are not 0. */
void sparc_li(struct code *c, int rd, long long v);
int sparc_li_len(long long v);

/* ---- memory ------------------------------------------------------------ */

/* rd = *(base + off): 1, 2, 4 or 8 bytes (8 is ldd: rd even, the pair rd,
 * rd+1, the word at the address into rd). `sign` picks ldsb/ldsh. */
void sparc_load(struct code *c, int rd, int base, int off, int size, int sign);
void sparc_store(struct code *c, int rd, int base, int off, int size);
/* The register-indexed forms: *(base + index). */
void sparc_load_rr(struct code *c, int rd, int base, int index, int size,
                   int sign);
void sparc_store_rr(struct code *c, int rd, int base, int index, int size);
/* The atomic ones: ldstub (a byte, set to 0xff), swap (a word), and
 * LEON3's casa [rs1] asi, rs2, rd (rd = old; stored rd's old value when
 * the word equals rs2). */
void sparc_ldstub(struct code *c, int rd, int base, int off);
void sparc_swap(struct code *c, int rd, int base, int off);
void sparc_casa(struct code *c, int rs1, int asi, int rs2, int rd);
void sparc_stbar(struct code *c);
void sparc_flush(struct code *c, int rs1, int off);

/* ---- the Y register and the state registers -------------------------- */

void sparc_rdy(struct code *c, int rd);                      /* rd %y, rd */
void sparc_wry(struct code *c, int rs1, int rs2);            /* wr rs1, rs2, %y */
void sparc_rdpsr(struct code *c, int rd);
void sparc_rdwim(struct code *c, int rd);
void sparc_rdtbr(struct code *c, int rd);
void sparc_wrpsr(struct code *c, int rs1, int rs2);
void sparc_wrwim(struct code *c, int rs1, int rs2);
void sparc_wrtbr(struct code *c, int rs1, int rs2);

/* ---- control flow ------------------------------------------------------- */

/* The integer condition codes, as Bicc's cond field numbers them. */
enum sparc_cond {
    SP_BN = 0, SP_BE = 1, SP_BLE = 2, SP_BL = 3, SP_BLEU = 4, SP_BCS = 5,
    SP_BNEG = 6, SP_BVS = 7, SP_BA = 8, SP_BNE = 9, SP_BG = 10, SP_BGE = 11,
    SP_BGU = 12, SP_BCC = 13, SP_BPOS = 14, SP_BVC = 15
};
/* The condition that is true exactly when `cond` is false. */
int sparc_cond_invert(int cond);
/* A Bicc whose target is `off` BYTES from the branch itself: a multiple
 * of 4 within -8 MiB .. 8 MiB - 4. `annul` sets the a bit. */
unsigned long sparc_enc_branch(int cond, int annul, long off);
/* Emitted with a zero displacement; returns its offset for sparc_patch_b. */
int sparc_b_placeholder(struct code *c, int cond, int annul);
/* Points the branch at `at` to `target` (offsets in the buffer). Returns 0
 * when the target is out of reach, which is the caller's to refuse. */
int sparc_patch_b(struct code *c, int at, int target);

/* call with a zero displacement, for an R_SPARC_WDISP30 relocation; and
 * the word for a known displacement in bytes from the call. */
void sparc_call(struct code *c);
unsigned long sparc_enc_call(long off);
/* jmpl rs1 + simm13, rd: `call reg` is rd = %o7, `ret` is %i7 + 8 into
 * %g0, `retl` %o7 + 8. */
void sparc_jmpl(struct code *c, int rd, int rs1, long long off);
void sparc_save(struct code *c, int rd, int rs1, long long imm);
void sparc_save_rr(struct code *c, int rd, int rs1, int rs2);
void sparc_restore(struct code *c, int rd, int rs1, int rs2);
void sparc_rett(struct code *c, int rs1, long long off);
/* Ticc: trap on `cond` with the trap number rs1 + imm7 (`ta 0`). */
void sparc_trap(struct code *c, int cond, int rs1, int imm7);
/* unimp imm22: after a call to a function returning a structure, the
 * structure's size; the callee returns past it. */
void sparc_unimp(struct code *c, unsigned long imm22);

#endif
