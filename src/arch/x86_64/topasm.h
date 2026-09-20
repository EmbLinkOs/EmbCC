#ifndef EMBCC_TOPASM_H
#define EMBCC_TOPASM_H

#include "../../parse/ast.h"

/* Assemble a file-scope __asm__ block into ta->code / syms / rels.
 *
 * The vocabulary has two halves. The DIRECTIVE half is arch-neutral and
 * always available: .global/.globl, labels (named and numeric-local), and
 * the data directives .byte/.long/.quad, which place bytes with no
 * interpretation. The MNEMONIC half -- `and $imm,%reg`, `call sym`,
 * `jmp local-label`, `ret` -- is x86-64, and `mnemonics_ok` is 0 on a
 * target whose instructions this file cannot encode. Anything outside
 * both halves is refused loudly (THE RULE). */
void topasm_assemble(struct topasm *ta, int mnemonics_ok);

#endif
