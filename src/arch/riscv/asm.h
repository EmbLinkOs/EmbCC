/* The RISC-V inline-asm assembler.
 *
 * Its counterpart on aarch64 (src/arch/aarch64/asm.h) has a vocabulary
 * MEASURED from the EmbLinkOS ARM kernel -- every template that build
 * preprocesses, 67 of them. There is no RISC-V corpus in this tree to
 * measure, so this one is derived from the privileged ISA instead: the
 * CSR instructions and the named registers a bare-metal program needs to
 * set up traps and interrupts, the fences, and the handful of
 * system instructions. That is a weaker basis and it is stated rather
 * than dressed up -- when RISC-V code exists here to measure, the
 * vocabulary should be pruned to what it actually uses.
 *
 * What does NOT change is the discipline: a statement outside the
 * vocabulary fails with a message naming it, never with a guessed
 * encoding (THE RULE). And every instruction is encoded through
 * src/arch/riscv/emit.c -- the same encoders the code generator uses and
 * tools/riscvcheck round-trips through llvm-mc -- so an inline asm
 * instruction cannot be encoded differently from a compiled one.
 *
 * Input is GNU RISC-V syntax with the operands ALREADY substituted
 * (irgen turns %0 and %[name] into register names first), so this sees
 * plain text such as "csrr a0, mhartid; fence". Statements separate on
 * ';' and newlines; '#' and "//" start a comment.
 */
#ifndef EMBCC_ARCH_RISCV_ASM_H
#define EMBCC_ARCH_RISCV_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded template to `out`. Returns 0, or -1 with a
 * NUL-terminated message in err[0..errlen). */
int rvasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* The register a name denotes: x0..x31, or an ABI name (zero, ra, sp, gp,
 * tp, t0-t6, s0-s11, a0-a7, and fp for s0). -1 for anything else. Used
 * for `register T v __asm__("a0")` variables and for clobber lists. */
int rvasm_gpr(const char *name, int len);

/* For the file assembler: is `w` an operand word of this statement -- a
 * CSR name after a csr instruction, a fence's iorw set -- rather than a
 * symbol? */
int rvasm_is_word(const char *stmt, const char *w, int wlen);

/* The inverse: the ABI name of a register number, for irgen to substitute
 * into a template. One name per register at one width -- RISC-V has no
 * w0/x0 question -- so this is the whole of operand rendering. */
const char *rv_reg_name(int reg);

/* Writes one assembly line per vocabulary entry -- every CSR name and
 * every instruction form -- so the golden test can hand the SAME lines to
 * llvm-mc and compare. Generated from the tables themselves, so an entry
 * added here cannot escape the referee. */
void rvasm_vocabulary(FILE *f);

#endif
