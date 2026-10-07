/* The LoongArch64 assembler: inline `__asm__` templates, and the
 * statements of a .s/.S file, a file-scope asm block or a naked function
 * through src/as/gas.c.
 *
 * There is no LoongArch corpus in this tree to measure a vocabulary from,
 * so it is what bare-metal LA64 code writes: the base integer
 * instructions the code generator uses and their neighbours (indexed
 * loads and stores, the bit-field and byte-reversal family, the AM*
 * atomics at every width they exist in), the barriers, and the privileged
 * instructions an RTOS needs -- csrrd/csrwr/csrxchg, ertn, idle,
 * cpucfg, the stable counter (rdtime*) and IOCSR access. The pseudo
 * instructions are llvm-mc's: nop, move, li.w/li.d, jr, ret, the swapped
 * branches (bgt, ble, bgtu, bleu) and those against zero (bltz, bgez,
 * bgtz, blez), and -- with a symbol -- la.pcrel/la.local, la/la.global,
 * la.abs, call36 and tail36. No floating-point or vector instruction:
 * the target is soft float. Anything else fails with a message naming it
 * (THE RULE).
 *
 * Every instruction is encoded through src/arch/loongarch/emit.c, the
 * encoder the code generator uses, and the whole vocabulary is compared
 * with llvm-mc by tests/golden/loongarch-asm.sh.
 *
 * The input is GNU LoongArch syntax with the operands already
 * substituted: a register is `$` and a number (`$r4`) or a psABI name
 * (`$a0`), a branch target a byte offset from the branch (`.+8`, or a
 * bare number). Statements separate on ';' and newlines; '#' and "//"
 * start a comment.
 */
#ifndef EMBCC_ARCH_LOONGARCH_ASM_H
#define EMBCC_ARCH_LOONGARCH_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded template to `out`. Returns 0, or -1 with a message
 * in err[0..errlen). */
int laasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* The register a name denotes -- "$r4", "$a0", "a0", "$fp", "$s9" -- or
 * -1. With or without the `$`, for register variables and clobber
 * lists. */
int laasm_gpr(const char *name, int len);

/* For src/as/gas.c: a `$`-register (a bare word in a .S file is a symbol;
 * a floating-point register is a register too, and its instructions are
 * refused by name), the word after a `%` (an operator), and the
 * statements that take a symbol with the relocations they carry. */
int laasm_is_reg(const char *name, int len);
int laasm_is_word(const char *stmt, const char *w, int len);
int laasm_symform(const char *stmt, struct asm_symform *f);

/* One line per vocabulary entry, for the referee. */
void laasm_vocabulary(FILE *f);

#endif
