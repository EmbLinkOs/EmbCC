/* The ColdFire assembler: inline `__asm__` templates, and the statements
 * of a .s/.S file, a file-scope asm block or a naked function through
 * src/as/gas.c.
 *
 * The vocabulary is the ISA_A+ the backend targets (the MCF5208's, QEMU's
 * mcf5208evb): move/movea/moveq, lea/pea, the .l ALU in all its
 * forms (addq/subq/addi/.../adda/addx), neg/not/swap/ext, clr/tst, the
 * shifts, the multiplies, divides and remainders, scc, bra/bsr/bcc in
 * their .s/.w forms, jmp/jsr/rts/rte, link/unlk, movem, the moves to
 * and from %sr, %ccr and %usp, movec, trap/stop/halt/nop/tpf/illegal, and
 * the bit instructions. A form outside it -- a rotate, dbcc, a 68000
 * byte-size add, ISA_B's mvs/mvz (the MCF5208 traps on them), the FPU's --
 * fails with a message naming it (THE RULE).
 *
 * Every instruction goes through emit.c's encoders, the ones the code
 * generator uses; tests/golden/coldfire-asm.sh referees the vocabulary
 * with QEMU's m68k disassembler (there is no GNU or LLVM m68k assembler
 * here) and runs what it assembles on the board.
 *
 * The input is GNU as's Motorola syntax, which m68k-elf GCC writes:
 * registers %d0-%d7, %a0-%a7, %fp (a6) and %sp (a7) -- the `%` optional,
 * either case, as GNU as takes them -- `#expr` immediates, `(An)`,
 * `(An)+`, `-(An)`, `(d,An)` or `d(An)`, `(d,An,Xi.l*s)`, `(d,%pc)`,
 * `(xxx).w`/`.l` and MIT's `An@`, `An@+`, `An@-`, `An@(d)`,
 * `An@(d,Xi:l:s)`; sizes as suffixes (`move.l`), `.w` where an
 * instruction has a choice and none is written, as GNU as defaults; a
 * transfer's target as `.+N` / `.-N` bytes from the instruction, or a
 * template's numeric label (`1:`, `1b`, `1f`). A branch without a size
 * takes the shortest that reaches, .s or .w (the MCF5208 has no .l). `move.l #n,Dn` with n
 * in -128..127 is moveq and `add`/`sub #1..8` addq/subq, as GNU as makes
 * them. Statements separate on newlines and `;`; `|` starts a comment, as
 * does a `#` at the start of a line.
 */
#ifndef EMBCC_ARCH_COLDFIRE_ASM_H
#define EMBCC_ARCH_COLDFIRE_ASM_H

#include <stdio.h>

#include "../code.h"

/* A template -- statements and numeric labels -- appended to `out`.
 * Returns 0, or -1 with a message in err[0..errlen). */
int cfasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* One statement, for src/as/gas.c, at the relaxation level the file
 * assembler has settled on for it (-1: the first pass's optimistic guess);
 * cfasm_took_level then says which form its branch took. */
int cfasm_encode(const char *text, struct code *out, char *err, int errlen);
void cfasm_set_level(int level);
int cfasm_took_level(void);

/* The register a name denotes -- d0-d7 (0-7), a0-a7 (8-15), fp, sp, with
 * or without a `%`, either case -- or -1. */
int cfasm_gpr(const char *name, int len);

/* For src/as/gas.c: a register; %sr, %ccr, %usp, %pc, a movec control
 * register or an index size where the statement has one; a transfer
 * (bra/bsr/bcc, whose label here is a displacement); and the statements
 * whose operand is a SYMBOL, with the relocation each carries. */
int cfasm_is_reg(const char *name, int len);
int cfasm_is_word(const char *stmt, const char *w, int len);
int cfasm_is_transfer(const char *stmt);
int cfasm_symform(const char *stmt, struct asm_symform *f);

/* Code alignment's padding: nops (and a zero byte for an odd gap). */
void cfasm_fill(struct code *out, long gap);

/* One line per vocabulary entry, for the referee: the statement, a `,
 * and the line QEMU's m68k disassembler prints for it, `{+N}` for the
 * address N bytes from its start (tools/cfasmcheck). */
void cfasm_vocabulary(FILE *f);

#endif
