/* The ARMv7-M inline-asm assembler.
 *
 * Its vocabulary is what a Cortex-M program reaches inline assembly FOR
 * and cannot say in C: the special registers (mrs/msr), the interrupt
 * masks (cpsid/cpsie), the barriers, the wait hints, the exclusive pair
 * that an atomic is built from, and the handful of arithmetic and
 * memory instructions a hand-written sequence mixes in with them. That
 * is the CMSIS core set -- the one every ARM C project's headers open
 * with -- rather than a general Thumb-2 assembler.
 *
 * A statement outside it fails with a message naming the statement,
 * never with a guessed encoding (THE RULE). And every instruction is
 * encoded through src/arch/thumb/emit.c, the same encoders the code
 * generator uses and tools/thumbcheck round-trips through
 * llvm-objdump, so inline asm and compiled code cannot disagree.
 *
 * ONE THING THIS DOES THAT THE RISC-V ONE NEED NOT. Thumb-2 has 16-bit
 * and 32-bit encodings of the same mnemonic, and which one an assembler
 * picks changes the size of the output. The encoders already choose --
 * t_alu_imm returns 0 when a value will not fit the narrow form, and
 * the caller widens -- so this file makes no width decision of its own.
 * A `.w` suffix is accepted and ignored for that reason: it is a request
 * for a width the encoder has already worked out.
 *
 * Input is GNU ARM syntax with the operands ALREADY substituted (irgen
 * turns %0 and %[name] into register names first), so this sees plain
 * text such as "mrs r0, primask; cpsid i". Statements separate on ';'
 * and newlines; '@' and "//" start a comment.
 */
#ifndef EMBCC_ARCH_THUMB_ASM_H
#define EMBCC_ARCH_THUMB_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded template to `out`. Returns 0, or -1 with a
 * NUL-terminated message in err[0..errlen). */
int tasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* The register a name denotes -- r0..r15, and sp/lr/pc for r13/r14/r15
 * -- or -1. Used for `register T v __asm__("r0")` and clobber lists. */
int tasm_gpr(const char *name, int len);

/* The inverse, for irgen to substitute into a template. */
const char *t_reg_name(int reg);

/* Writes one assembly line per vocabulary entry, so the golden test can
 * hand the SAME lines to llvm-mc and compare. Generated from the tables
 * themselves: an entry added here cannot escape the referee. */
void tasm_vocabulary(FILE *f);

#endif
