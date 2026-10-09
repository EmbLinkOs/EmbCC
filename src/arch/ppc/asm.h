/* The 32-bit PowerPC assembler: inline `__asm__` templates, and the
 * statements of a .s/.S file, a file-scope asm block or a naked function
 * through src/as/gas.c.
 *
 * The vocabulary is the integer unit of an e500 (the ppce500 board's
 * core) and of the classic 32-bit cores it shares with: the XO-form
 * arithmetic with its OE and record (`.`) forms, the logical and shift
 * X-forms, the D-form immediates, rlwinm/rlwimi/rlwnm and their shift
 * and rotate mnemonics, every load and store with its update and indexed
 * forms, lmw/stmw, the byte-reversed ones and lwarx/stwcx., compares
 * into any CR field, the CR logic, isel, tw/twi and the trap mnemonics,
 * b/ba/bl/bla, bc/bclr/bcctr with BO and BI and the simplified branch
 * mnemonics (beq, bne, blt, bdnz, blr, bctr, bctrl, beqlr...), mfspr and
 * mtspr by number or by name and the mfNAME/mtNAME forms, mfmsr/mtmsr,
 * wrtee/wrteei, mfcr/mtcrf, the syncs (sync, msync, lwsync, isync, eieio,
 * mbar), the cache and TLB operations, sc, rfi/rfci/rfmci -- and li, lis,
 * la, mr, not, nop, sub and the other extended mnemonics GNU as accepts.
 * A form outside that list fails with a message naming it (THE RULE):
 * floating point, AltiVec and SPE among them (EmbCC compiles soft float),
 * and the branch-prediction suffixes `+` and `-`.
 *
 * Every instruction is packed by src/arch/ppc/emit.c's format encoders,
 * with the opcodes emit.h names; tests/golden/ppc-asm.sh compares every
 * form of the vocabulary with llvm-mc (-mcpu=e500) byte for byte and
 * checks the mnemonic llvm-objdump reads back.
 *
 * The input is GNU PowerPC syntax with the operands substituted:
 * registers as numbers (GCC's spelling), rN or %rN (and sp); CR fields as
 * crN or a number, CR bits as expressions of crN, lt, gt, eq, so and un;
 * constants as C expressions, with @ha, @h and @l; a branch target as
 * `.+N` / `.-N`, the byte distance from the instruction. Inside a
 * template numeric labels (`1:`, `1b`, `1f`) work as in GCC's; any other
 * label is the file assembler's. Statements separate on ';' and
 * newlines; '#' and "//" start a comment.
 *
 * `%_N` is register N spelt by irgen for an operand it substituted: the
 * same register, but not one the template NAMES (ppcasm_named).
 */
#ifndef EMBCC_ARCH_PPC_ASM_H
#define EMBCC_ARCH_PPC_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded template to `out`. Returns 0, or -1 with a message
 * in err[0..errlen). */
int ppcasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* After ppcasm_assemble: the general registers the text named outright
 * (bit r), and whether it links -- a branch with LK set, or a write of
 * LR -- so the function holding it must save its own. */
unsigned long ppcasm_named(void);
int ppcasm_links(void);

/* A general register by name -- r0-r31 or %r0-%r31, sp, or (for a
 * clobber list, as GCC's) a bare number -- or -1. */
int ppcasm_gpr(const char *name, int len);

/* For src/as/gas.c: a register (so `r3` is not a symbol); an operand word
 * (a CR field or bit name, the operator after `@`, an SPR's name where
 * mfspr/mtspr take one); and the statements that name a symbol -- a
 * branch to one this file does not resolve (symform), and the @ha/@h/@l
 * operands that name a symbol's ADDRESS, relocated whatever the symbol
 * is (symform_abs) -- with the R_PPC_* each carries. */
int ppcasm_is_reg(const char *name, int len);
int ppcasm_is_word(const char *stmt, const char *w, int len);
int ppcasm_symform(const char *stmt, struct asm_symform *f);
int ppcasm_symform_abs(const char *stmt, struct asm_symform *f);

/* The SPR a name denotes (lr, ctr, xer, srr0, sprg0, dec, ...), or -1. */
int ppcasm_spr(const char *name, int len);

/* One line per vocabulary entry, for the referee: the statement, an @,
 * and the mnemonic llvm-objdump prints for it; then, for the SPR names
 * llvm-mc knows a simplified mnemonic for, `=` lines pairing the two
 * spellings (see tools/ppcasmcheck). */
void ppcasm_vocabulary(FILE *f);

#endif
