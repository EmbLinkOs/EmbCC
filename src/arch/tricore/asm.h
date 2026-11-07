/* The TriCore assembler (src/arch/tricore/asm.c). */
#ifndef EMBCC_ARCH_TRICORE_ASM_H
#define EMBCC_ARCH_TRICORE_ASM_H

#include <stddef.h>

#include "../code.h"

/* Assemble a template's text (operands already substituted) into c.
 * Returns 0, or -1 with a message naming the statement in err. */
int tcasm_assemble(const char *text, struct code *c, char *err, size_t errlen);
/* A register by name -- d0-d15, a0-a15, sp, e0-e14, with or without a
 * leading % -- its number, its file ('d', 'a' or 'e') in *file; -1 when
 * the n characters at p name none. */
int tcasm_reg(const char *p, int n, int *file);

/* For src/as/gas.c: tcasm_assemble with gas_target's signature; a
 * register (any file); `hi`/`lo` and a core register's name as operand
 * words; and the statements whose operand is a symbol, with the
 * relocation each carries (see asm.c). */
int tcasm_encode(const char *text, struct code *c, char *err, int errlen);
int tcasm_is_reg(const char *name, int len);
int tcasm_is_word(const char *stmt, const char *w, int len);
int tcasm_symform(const char *stmt, struct asm_symform *f);
/* A branch or call whose label substitution resolves (gas_target's
 * is_transfer); relaxation's level (set_level/took_level: 1 is the long
 * branch); the start of a pass; `.code16`/`.code32` (16 or 32): the next
 * instruction's width. */
int tcasm_is_transfer(const char *stmt);
void tcasm_set_level(int level);
int tcasm_took_level(void);
void tcasm_reset(void);
void tcasm_width(int bits);

#endif
