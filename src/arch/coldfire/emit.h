/* ColdFire (ISA_A) instruction encoding: the 68000 family's variable-length
 * format -- a 16-bit operation word and up to four 16-bit extension words,
 * all big-endian -- restricted to what a ColdFire core executes
 * (docs/internals/coldfire-plan.md).
 *
 * Nearly every instruction names its operands through one 6-bit EFFECTIVE
 * ADDRESS field (mode and register) and the extension words that mode
 * needs, so this file packs an effective address once (struct cf_ea) and
 * every instruction is its operation word plus which operand goes in which
 * field. ColdFire is the 68000 cut down: integer arithmetic is .l only
 * (add/sub/and/or/eor/cmp/neg/not), shifts are register-only, a move may
 * not combine two long effective addresses, multiply and divide take only
 * the simple memory modes, and there is no predecrement movem, no rotate,
 * no dbcc. Each wrapper checks the modes its instruction allows ON COLDFIRE
 * and stops with an internal error rather than emit a 68000 form a ColdFire
 * core would trap on.
 *
 * tools/cfcheck prints every form with QEMU's disassembly of it
 * (tests/golden/coldfire-encoding.sh).
 */
#ifndef EMBCC_ARCH_COLDFIRE_EMIT_H
#define EMBCC_ARCH_COLDFIRE_EMIT_H

#include "../code.h"

/* Registers: d0-d7 are 0-7, a0-a7 8-15 -- the DWARF numbering as well.
 * a6 is the frame pointer (link/unlk), a7 the stack pointer. */
enum {
    CF_D0 = 0, CF_D1, CF_D2, CF_D3, CF_D4, CF_D5, CF_D6, CF_D7,
    CF_A0 = 8, CF_A1, CF_A2, CF_A3, CF_A4, CF_A5, CF_A6, CF_A7
};
#define CF_FP CF_A6
#define CF_SP CF_A7
#define CF_IS_A(r) ((r) >= 8)

/* The register's name as QEMU's (binutils') disassembler spells it: d0..d7,
 * a0..a5, fp, sp. */
const char *cf_reg_name(int r);

/* ---- effective addresses --------------------------------------------- */

enum cf_mode {
    CFM_D,        /* Dn */
    CFM_A,        /* An */
    CFM_IND,      /* (An) */
    CFM_POST,     /* (An)+ */
    CFM_PRE,      /* -(An) */
    CFM_DISP,     /* (d16,An) */
    CFM_IDX,      /* (d8,An,Xi.l*scale), scale 1, 2 or 4 */
    CFM_ABSW,     /* (xxx).w */
    CFM_ABSL,     /* (xxx).l */
    CFM_PCDISP,   /* (d16,PC): disp measured from the extension word */
    CFM_PCIDX,    /* (d8,PC,Xi.l*scale) */
    CFM_IMM       /* #imm, as wide as the operation */
};

struct cf_ea {
    enum cf_mode mode;
    int reg;          /* the register, 0-15 (An modes: 8-15) */
    long disp;        /* DISP/IDX/PC*: the displacement; ABS*: the address */
    int xreg;         /* IDX/PCIDX: the index register (0-15), always .l */
    int xscale;       /* IDX/PCIDX: 1, 2 or 4 */
    long imm;         /* IMM */
};

struct cf_ea cf_dreg(int r);
struct cf_ea cf_areg(int r);
struct cf_ea cf_ind(int a);
struct cf_ea cf_post(int a);
struct cf_ea cf_pre(int a);
struct cf_ea cf_disp(int a, long d);          /* (An) when d is 0 */
struct cf_ea cf_disp16(int a, long d);        /* always (d16,An) */
struct cf_ea cf_idx(int a, long d, int x, int scale);
struct cf_ea cf_absl(long addr);
struct cf_ea cf_pcdisp(long d);
struct cf_ea cf_imm(long v);

/* What kind of operand an ea is. */
int cf_ea_is_reg(const struct cf_ea *e);          /* Dn or An */
int cf_ea_is_mem_alterable(const struct cf_ea *e);/* (An) .. (xxx).l */
/* How many bytes an operand's extension words take at this size. */
int cf_ea_ext_len(const struct cf_ea *e, int size);

/* The 6-bit field and the extension words. Exposed for the checker and
 * the linker's stub; the instructions below are the encoder's interface. */
int cf_ea_field(const struct cf_ea *e);
void cf_ea_ext(struct code *c, const struct cf_ea *e, int size);

/* ---- the code buffer: 16-bit words, big-endian --------------------- */

void cf_w(struct code *c, unsigned v);
void cf_l(struct code *c, unsigned long v);
unsigned cf_rdw(const struct code *c, int at);
void cf_wrw(struct code *c, int at, unsigned v);
void cf_wrl(struct code *c, int at, unsigned long v);
/* The same in plain memory, for the linker. */
unsigned long cf_get32(const unsigned char *p);
void cf_put32(unsigned char *p, unsigned long v);
void cf_put16(unsigned char *p, unsigned v);

/* ---- moves ----------------------------------------------------------- */

/* move.b/.w/.l src,dst (movea.w/.l when dst is An). Refuses the
 * combinations ColdFire does not encode: a source with a 16-bit
 * displacement into an indexed or absolute destination, and an indexed,
 * absolute or immediate source into anything but a register or (An),
 * (An)+, -(An). */
void cf_move(struct code *c, int size, struct cf_ea src, struct cf_ea dst);
/* Would cf_move accept it? */
int cf_move_ok(int size, const struct cf_ea *src, const struct cf_ea *dst);
void cf_moveq(struct code *c, long v, int dn);       /* -128..127 */
void cf_lea(struct code *c, struct cf_ea src, int an);
void cf_pea(struct code *c, struct cf_ea src);
/* movem.l: `mask` bit k is register k (d0 = bit 0, a7 = bit 15); ea is
 * (An) or (d16,An) -- ColdFire has no predecrement or postincrement form */
void cf_movem_store(struct code *c, unsigned mask, struct cf_ea dst);
void cf_movem_load(struct code *c, struct cf_ea src, unsigned mask);

/* ---- arithmetic and logic: .l only ----------------------------------- */

enum cf_alu { CF_ADD, CF_SUB, CF_AND, CF_OR, CF_EOR, CF_CMP };

/* op.l <ea>,Dn (not EOR, whose source is always a data register) */
void cf_alu(struct code *c, enum cf_alu op, struct cf_ea src, int dn);
/* op.l Dn,<ea> into memory (ADD, SUB, AND, OR, EOR) */
void cf_alu_mem(struct code *c, enum cf_alu op, int dn, struct cf_ea dst);
/* adda.l / suba.l / cmpa.l <ea>,An (ADD, SUB, CMP) */
void cf_alua(struct code *c, enum cf_alu op, struct cf_ea src, int an);
/* addi/subi/andi/ori/eori/cmpi.l #imm,Dn -- ColdFire's only immediate
 * forms, into a data register */
void cf_alu_imm(struct code *c, enum cf_alu op, long imm, int dn);
/* addq.l / subq.l #1..8,<ea> (Dn, An or memory) */
void cf_addq(struct code *c, int sub, int n, struct cf_ea dst);
/* addx.l / subx.l Dy,Dx */
void cf_addx(struct code *c, int sub, int dy, int dx);

enum cf_un { CF_NEG, CF_NEGX, CF_NOT, CF_SWAP, CF_EXTW, CF_EXTL, CF_EXTBL };
void cf_unary(struct code *c, enum cf_un op, int dn);
void cf_clr(struct code *c, int size, struct cf_ea dst);
void cf_tst(struct code *c, int size, struct cf_ea src);

enum cf_sh { CF_ASL, CF_ASR, CF_LSL, CF_LSR };
void cf_shift_imm(struct code *c, enum cf_sh op, int n, int dn);  /* 1..8 */
void cf_shift_reg(struct code *c, enum cf_sh op, int dcount, int dn);

/* muls.w/mulu.w <ea>,Dn (16 x 16 -> 32) and muls.l/mulu.l <ea>,Dn
 * (32 x 32 -> the low 32), size 2 or 4 */
void cf_mul(struct code *c, int sign, int size, struct cf_ea src, int dn);
/* divs.l/divu.l <ea>,Dq: Dq / ea -> Dq */
void cf_div(struct code *c, int sign, struct cf_ea src, int dq);
/* rems.l/remu.l <ea>,Dr:Dq: Dq % ea -> Dr, Dq unchanged; Dr != Dq */
void cf_rem(struct code *c, int sign, struct cf_ea src, int dr, int dq);

/* ---- conditions and control ------------------------------------------ */

enum cf_cond {
    CF_T = 0, CF_F = 1, CF_HI = 2, CF_LS = 3, CF_CC = 4, CF_CS = 5,
    CF_NE = 6, CF_EQ = 7, CF_VC = 8, CF_VS = 9, CF_PL = 10, CF_MI = 11,
    CF_GE = 12, CF_LT = 13, CF_GT = 14, CF_LE = 15
};
int cf_cond_invert(int cond);
void cf_scc(struct code *c, int cond, int dn);

/* bcc.w / bra.w with displacement `disp` from the extension word (the
 * operation word's address + 2), and the .b form when `disp` fits 8 bits
 * and is not 0 or -1. cond CF_T is bra; CF_F (bsr) is not a branch. */
void cf_bcc_w(struct code *c, int cond, long disp);
void cf_bcc_b(struct code *c, int cond, long disp);
/* A bcc.w whose displacement is patched later: returns its offset. */
int cf_bcc_placeholder(struct code *c, int cond);
/* Point the bcc.w at `at` to `target` (an offset in the same buffer);
 * 0 when it does not reach. */
int cf_patch_bcc(struct code *c, int at, int target);
void cf_bsr_w(struct code *c, long disp);

void cf_jsr(struct code *c, struct cf_ea target);
void cf_jmp(struct code *c, struct cf_ea target);
void cf_rts(struct code *c);
void cf_nop(struct code *c);
void cf_illegal(struct code *c);
void cf_halt(struct code *c);
void cf_trap(struct code *c, int vec);
void cf_link(struct code *c, int an, long disp);   /* link.w: -32768..32767 */
void cf_unlk(struct code *c, int an);

#endif
