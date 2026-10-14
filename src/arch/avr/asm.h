/* The AVR assembler.
 *
 * Two callers, one parser. `avrasm_assemble` takes GNU AVR syntax with
 * every operand already a number or a register and returns the bytes --
 * which is what a `.S` file needs (src/as/gas.c substitutes labels into
 * `.±N` displacements first) and what inline `__asm__` needs (irgen
 * substitutes %0 and %[name] into register names first). Neither sees a
 * symbol, because neither can resolve one.
 *
 * The forms that DO name a symbol are the assembler's other half:
 * `avrasm_symform` recognises them, hands back a statement it can encode
 * with a zero field, and says which relocation the site needs. AVR has
 * more of these than any other target here, because a sixteen-bit address
 * is loaded a BYTE AT A TIME -- `ldi rlo, lo8(sym)` and `ldi rhi,
 * hi8(sym)` are two instructions and two relocations for one address --
 * and because program space is addressed in words, so a function's
 * address needs the `gs()` forms rather than the data ones.
 *
 * Every instruction is encoded through src/arch/avr/emit.c, the same
 * encoders the code generator uses and tools/avrcheck refereed against
 * llvm-mc. So an instruction in a `.S` file cannot be encoded differently
 * from a compiled one, and a mistake in a split operand field is caught
 * in one place rather than two.
 *
 * A statement outside the vocabulary fails with a message naming it, never
 * with a guessed encoding (THE RULE). On this machine that matters more
 * than most: nearly every wrong guess here is a VALID instruction.
 */
#ifndef EMBCC_ARCH_AVR_ASM_H
#define EMBCC_ARCH_AVR_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded statements to `out`. Statements separate on ';' and
 * newlines; '#' and ';' at the start of a line and "//" anywhere start a
 * comment. Returns 0, or -1 with a NUL-terminated message in
 * err[0..errlen). */
int avrasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* The register a name denotes: r0..r31, the pointer halves XL/XH, YL/YH,
 * ZL/ZH, and the avr-libc names __tmp_reg__ (r0), __zero_reg__ (r1) and
 * __SREG__ (which is not a register and answers -1). -1 for anything that
 * is not a register, which is how the assembler tells `r20` from a label. */
int avrasm_gpr(const char *name, int len);
/* Is W, in statement STMT, a word that names no symbol: avr-gcc's
 * __SREG__, __SP_H__ and __SP_L__ (src/as/gas.c's is_word). */
int avrasm_is_word(const char *stmt, const char *w, int len);

/* ---- the forms that name a symbol ------------------------------------ */

/* Does `stmt` name a symbol, and how? Returns 1 and fills `f`, or 0 when
 * the statement is not one of these forms. The symbol is not looked up
 * here -- that is the caller's table -- so this works the same for a label
 * defined in the file and one that is not. */
int avrasm_symform(const char *stmt, struct asm_symform *f);

/* Writes one assembly line per vocabulary entry, so the golden test can
 * hand the SAME lines to llvm-mc and compare the bytes. Generated from the
 * table itself, so a mnemonic added here cannot escape the referee -- which
 * is the discipline the branch-condition bug bought. */
void avrasm_vocabulary(FILE *f);

/* The PC-relative forms, separately: llvm-mc relocates a branch even to a
 * label in its own section, so its bytes are a placeholder and a byte
 * comparison would grade nothing. These are assembled here and
 * DISASSEMBLED, and the text compared -- the same arrangement emit.c uses,
 * and for the same reason it exists. */
void avrasm_pcrel_vocabulary(FILE *f);
int  avrasm_pcrel_encode(struct code *out, char *err, int errlen);

#endif
