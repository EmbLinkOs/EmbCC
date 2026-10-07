/* The MIPS32r2 inline-asm assembler.
 *
 * There is no MIPS corpus in this tree to measure a vocabulary from, so
 * it is what a bare-metal PIC32-class program writes in an asm statement:
 * the integer, multiply/divide, load/store, bit-manipulation and
 * synchronisation instructions, coprocessor 0 (mfc0/mtc0, di/ei, ehb,
 * eret, wait), traps, jr/jalr, and branches with a numeric displacement
 * (bytes from the delay slot -- a template cannot see the function's
 * labels). The pseudo-instructions move, li, not, negu, b, beqz and
 * bnez are accepted; gas's macros (the trapping neg, a div with a
 * destination) are not. Anything else fails with a message naming it
 * (THE RULE).
 *
 * Every instruction is encoded through src/arch/mips/emit.c, the encoder
 * the code generator uses and tools/mipscheck referees, and the whole
 * vocabulary is compared with llvm-mc by tests/golden/mips-asm.sh.
 *
 * The input is GNU MIPS syntax with the operands already substituted:
 * registers are `$` and a number or an o32 name. Statements separate on
 * ';' and newlines; '#' and "//" start a comment. A template starts in
 * `.set reorder` mode, as GCC's and clang's do: a nop follows every
 * branch and jump, unless `.set noreorder` makes the delay slots the
 * template's own.
 */
#ifndef EMBCC_ARCH_MIPS_ASM_H
#define EMBCC_ARCH_MIPS_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded template to `out`, starting in `.set reorder` mode
 * as GCC's and clang's inline asm does. Returns 0, or -1 with a message
 * in err[0..errlen). */
int mipsasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* The same, in whatever mode the statements before left (`.set
 * noreorder`, `.set push`): the file assembler's entry point, one
 * statement at a time. mipsasm_reset starts a pass over in reorder
 * mode. */
int mipsasm_encode(const char *text, struct code *out, char *err, int errlen);
void mipsasm_reset(void);

/* For src/as/gas.c: a `$`-register (a bare `sp` in a .S file is a
 * symbol), the word after a `%` operator, and the statements that take a
 * symbol (jal/j, %hi/%lo, la) with the relocations they carry. */
int mipsasm_is_reg(const char *name, int len);
int mipsasm_is_word(const char *stmt, const char *w, int len);
int mipsasm_symform(const char *stmt, struct asm_symform *f);

/* The register a name denotes -- "$4", "$a0", "a0", "$sp", "$s8"... -- or
 * -1. For register variables and clobber lists. */
int mipsasm_gpr(const char *name, int len);

/* One line per vocabulary entry, for the referee. */
void mipsasm_vocabulary(FILE *f);

#endif
