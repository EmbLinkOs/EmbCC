/* The SPARC V8 assembler: inline `__asm__` templates, and the statements
 * of a .s/.S file, a file-scope asm block or a naked function through
 * src/as/gas.c.
 *
 * The vocabulary is the integer unit of a LEON3: the ALU in its register
 * and simm13 forms (with the condition-code, tagged and multiply-step
 * forms), loads and stores of every width and their alternate-space
 * forms, ldstub, swap and LEON's casa, sethi with %hi()/%lo(), every
 * Bicc with its `,a` annul bit, call, jmpl, rett, save and restore, rd
 * and wr of %y, %psr, %wim, %tbr and %asr1-%asr31, every Ticc, flush,
 * stbar, unimp and nop -- and the synthetic instructions GNU as accepts
 * for them: mov, cmp, tst, not, neg, inc/dec(cc), clr(b/h), btst, bset,
 * bclr, btog, set, jmp, ret, retl, b and the condition synonyms. A form
 * outside that list fails with a message naming it (THE RULE):
 * floating-point and coprocessor instructions (EmbCC compiles soft float)
 * and SPARC V9's forms among them.
 *
 * Every instruction is packed by src/arch/sparc/emit.c's format encoders,
 * with the op3 values emit.h names; tests/golden/sparc-asm.sh compares
 * every form of the vocabulary with llvm-mc byte for byte and checks the
 * mnemonic llvm-objdump reads back.
 *
 * The input is GNU SPARC syntax with the operands substituted: registers
 * %g0-%g7, %o0-%o7, %l0-%l7, %i0-%i7, %r0-%r31, %sp and %fp; constants
 * as C expressions, %hi(c) and %lo(c) of a constant; a branch's or a
 * call's target as `.+N` / `.-N`, the byte distance from the
 * instruction. Delay slots are the programmer's: nothing is reordered
 * and nothing is filled. Inside a template numeric labels (`1:`, `1b`,
 * `1f`) work as in GCC's; any other label is the file assembler's.
 * Statements separate on ';' and newlines; '!' and "//" start a comment.
 */
#ifndef EMBCC_ARCH_SPARC_ASM_H
#define EMBCC_ARCH_SPARC_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded template to `out`. Returns 0, or -1 with a message
 * in err[0..errlen). */
int spasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* The integer register a name denotes -- with or without its `%`:
 * g0-g7, o0-o7, l0-l7, i0-i7, r0-r31, sp, fp -- or -1. */
int spasm_gpr(const char *name, int len);

/* For src/as/gas.c: an operand word (a register or an operator after
 * `%`, the `a` of `,a`) rather than a symbol; the statements that name a
 * symbol -- a call or a branch to one defined elsewhere (symform), and
 * the forms that name a symbol's ADDRESS, %hi()/%lo() and `set`, which
 * are relocated whatever the symbol is (symform_abs) -- with the
 * R_SPARC_* each carries. */
int spasm_is_word(const char *stmt, const char *w, int len);
int spasm_symform(const char *stmt, struct asm_symform *f);
int spasm_symform_abs(const char *stmt, struct asm_symform *f);

/* Does the template call -- `call`, or a jmpl that links through %o7? A
 * call changes %o0-%o7 and %g1-%g4, which irgen keeps operands and live
 * values out of. */
int spasm_template_calls(const char *text);

/* One line per vocabulary entry, for the referee: the statement, an @,
 * and the mnemonic llvm-objdump prints for it (see tools/spasmcheck). */
void spasm_vocabulary(FILE *f);

#endif
