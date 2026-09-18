/* The aarch64 inline-asm assembler.
 *
 * The x86 backend assembles inline asm inside irgen (asm_assemble) with the
 * kernel's full x86 vocabulary. This is its aarch64 counterpart, and it is
 * deliberately NOT a general assembler: its vocabulary is exactly what the
 * EmbLinkOS ARM kernel's inline asm uses — measured, not guessed, by
 * preprocessing every C file the aarch64 kernel build compiles and collecting
 * every template (67 distinct). A statement outside it fails with a message
 * naming the statement, never with a guessed encoding (THE RULE).
 *
 * Input is GNU aarch64 syntax with the operands ALREADY substituted (irgen
 * turns %0 / %w0 / %x0 / %[name] into registers and immediates first), so this
 * sees plain text such as "isb; mrs x9, cntvct_el0". Statements separate on
 * ';' and newlines; `//` starts a comment.
 */
#ifndef EMBCC_ASM_ASM_ARM64_H
#define EMBCC_ASM_ASM_ARM64_H

#include <stdio.h>

#include "emit.h"

/* Appends the encoded template to `out`. Returns 0, or -1 with a
 * NUL-terminated message in err[0..errlen). */
int a64asm_assemble(const char *text, struct code *out, char *err, int errlen);

/* The register a name denotes on aarch64 — x0..x30 / w0..w30 give 0..30,
 * xzr/wzr 31 — or -1. Used for `register T v __asm__("x0")` variables and
 * for clobber lists. */
int a64asm_gpr(const char *name, int len);

/* Writes one assembly line per vocabulary entry — every named system
 * register, tlbi operation, barrier option and hint — so the golden test can
 * hand the SAME lines to aarch64-elf-as and compare. Generated from the
 * tables themselves: an entry added here cannot escape the referee. */
void a64asm_vocabulary(FILE *f);

#endif
