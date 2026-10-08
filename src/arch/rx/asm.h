/* The RX assembler: inline `__asm__` templates, and the statements of a
 * .s/.S file, a file-scope asm block or a naked function through
 * src/as/gas.c.
 *
 * The vocabulary is RXv1's integer instruction set as GNU as spells it:
 * mov/movu in every size and addressing mode (register, #imm, dsp[rN],
 * [ri, rb], [rN+], [-rN]), the ALU with registers, immediates and memex
 * memory operands (`add 4[r1].w, r2`), the three-operand forms, the
 * shifts and rotates, push/pop/pushm/popm/pushc/popc, bra/bsr/jmp/jsr/
 * rts/rtsd, every conditional branch, mvtc/mvfc/mvtipl/setpsw/clrpsw,
 * int/rte/rtfi/wait/brk, the bit instructions on registers and on bytes
 * in memory, bmCND and scCND, the string instructions, sat/satr and the
 * accumulator (mvfachi... racw, mulhi... maclo). A form outside it --
 * the FPU's, a memory-to-memory mov -- fails with a message naming it
 * (THE RULE).
 *
 * Every instruction goes through emit.c's encoders, the ones the code
 * generator uses; tests/golden/rx-asm.sh referees the vocabulary byte for
 * byte against GNU as (rx-elf-as) and its text against rx-elf-objdump.
 *
 * The input is GNU RX syntax with the operands substituted: registers
 * r0-r15 (GNU has no `sp`), immediates `#expr`, memory `dsp[rN]` with an
 * optional `.b/.w/.l/.ub/.uw` on a memex source, and a transfer's target
 * as `.+N` / `.-N` -- the byte distance from this instruction, which is
 * where both GNU's `.` and RX's displacements start. A branch written
 * without a size takes the shortest form that reaches, as GNU as's
 * relaxation does: bra .s/.b/.w/.a, bsr .w/.a, beq/bne .s/.b/.w and then
 * the inverse over a bra.a, any other condition .b and then its inverse
 * over a bra.w or bra.a. Inside a template numeric labels (`1:`, `1b`,
 * `1f`) work as in GCC's; any other label is the file assembler's.
 * Statements separate on newlines and `!`; `;` starts a comment (GNU's RX
 * comment character), as does a `#` at the start of a line.
 */
#ifndef EMBCC_ARCH_RX_ASM_H
#define EMBCC_ARCH_RX_ASM_H

#include <stdio.h>

#include "../code.h"

/* Assembles a template -- statements, numeric labels -- appending to
 * `out`. Returns 0, or -1 with a message in err[0..errlen). */
int rxasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* One statement, for src/as/gas.c, at the relaxation level the file
 * assembler has settled on for it (rxasm_set_level: -1 the optimistic
 * first guess, else the least form index a branch may take); after it,
 * rxasm_took_level says which form a branch took (0 for anything else). */
int rxasm_encode(const char *text, struct code *out, char *err, int errlen);
void rxasm_set_level(int level);
int rxasm_took_level(void);

/* The general register a name denotes -- r0..r15, either case -- or -1. */
int rxasm_gpr(const char *name, int len);

/* For src/as/gas.c: a register; a control register, PSW flag or memex
 * size where the statement takes one; a transfer (whose symbol is a
 * displacement when this file defines it); and the statements whose
 * operand is a SYMBOL, with the relocation each carries. */
int rxasm_is_reg(const char *name, int len);
int rxasm_is_word(const char *stmt, const char *w, int len);
int rxasm_is_transfer(const char *stmt);
int rxasm_symform(const char *stmt, struct asm_symform *f);

/* GNU as's padding for `gap` bytes of code (1..127): the multi-byte nops,
 * or a bra.b over the rest. Appended to out. */
void rxasm_fill(struct code *out, long gap);

/* One line per vocabulary entry, for the referee: the statement, an @,
 * and what rx-elf-objdump prints for it -- `|` between the instructions
 * of a statement that is two, `{+N}` for the address N bytes from it. */
void rxasm_vocabulary(FILE *f);

#endif
