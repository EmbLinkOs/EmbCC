/* x86-64's AT&T-syntax statement assembler: what src/as/gas.c drives for
 * a `.s` / `.S` file on an x86-64 ELF target, and what an inline asm
 * template the original vocabulary (irgen.c's asm_assemble) does not
 * cover is assembled with, its operands already substituted.
 *
 * GNU as's syntax: `op src, dst`, `%reg`, `$imm`, `disp(base, index,
 * scale)`, a b/w/l/q suffix where no register says the size, `*` before
 * an indirect jump's or call's target, `lock`/`rep` prefixes. A branch's
 * target and a RIP-relative displacement arrive as `.+N`, from the
 * statement's start, as every target's statement assembler takes them;
 * a symbol is the file assembler's (x86att_symform). Encodings are
 * llvm-mc's (tests/golden/x86-att-asm.sh compares them byte for byte). */
#ifndef EMBCC_X86_ATT_H
#define EMBCC_X86_ATT_H

#include "../code.h"

int x86att_assemble(const char *text, struct code *out, char *err, int errlen);
int x86att_is_reg(const char *name, int len);
int x86att_is_word(const char *stmt, const char *w, int len);
int x86att_symform(const char *stmt, struct asm_symform *f);
int x86att_is_transfer(const char *stmt);
/* jmp/jcc relaxation, gas.c's levels: -1 (pass 1) or 0 lets a branch be
 * its two-byte form where it reaches; 1 makes it the four-byte one. */
void x86att_set_level(int level);
int x86att_took_level(void);

#endif
