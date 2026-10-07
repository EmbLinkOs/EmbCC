/* TriCore inline-asm vocabulary (src/arch/tricore/asm.c). */
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

#endif
