/* A GNU-syntax file assembler: `.S` and `.s` inputs, for every target
 * whose instructions EmbCC can already encode.
 *
 * WHY THIS EXISTS. Firmware starts in assembly -- a reset vector, the
 * stack set up before C can run, a context switch -- and EmbCC could
 * not compile a single line of it for any embedded target. `embas`
 * assembles NASM syntax for x86-64 only, and the per-target asm.c
 * files assemble one STATEMENT at a time for inline `__asm__`, with
 * the operands already substituted and no idea what a label is.
 *
 * WHAT IS SHARED AND WHAT IS NOT. Everything about a FILE is here and
 * is target-independent: sections, labels, the directives, the symbol
 * table, relocations, and writing the object. Only instruction
 * encoding is delegated, to the same asm.c the compiler's inline
 * assembly already goes through -- so a mnemonic cannot assemble one
 * way in a .S file and another inside __asm__, which is the bug a
 * second assembler per target would eventually have.
 *
 * HOW A LABEL REACHES AN INSTRUCTION. The statement assemblers take a
 * PC-relative displacement, spelled `.+N`, and know nothing else. This
 * driver runs two passes: the first places every label by encoding
 * each instruction with a zero displacement to learn its size, the
 * second substitutes the real displacement and encodes for good. A
 * reference to a symbol this file does not define becomes a
 * relocation instead, and the forms that can carry one are named
 * explicitly rather than guessed at.
 */
#ifndef EMBCC_AS_GAS_H
#define EMBCC_AS_GAS_H

#include "../arch/code.h"

/* What one target contributes: how to encode a statement, and what to
 * call itself in an ELF header. `encode` is the target's existing
 * inline-asm entry point. */
struct gas_target {
    int machine;                    /* ELF e_machine */
    int is32;                       /* ELFCLASS32 */
    int (*encode)(const char *text, struct code *out, char *err, int errlen);
    /* Is this identifier one of the target's register names? Without
     * it the driver cannot tell `a0` from a label, and would try to
     * relocate against a register. The targets already answer this for
     * inline asm's clobber lists. */
    int (*is_reg)(const char *name, int len);
    /* The relocation types this target uses for the two forms that can
     * name an external symbol. 0 means "this target has no such form",
     * and the driver then refuses by name. */
    int r_call;                     /* `call sym`  -- a two-instruction pair */
    int r_pcrel_hi, r_pcrel_lo;     /* `la rd, sym` -- likewise */
    int r_abs32, r_abs64;           /* `.word sym` / `.quad sym` */
};

/* Assembles `in_path` into an ET_REL object at `out_path`, for the
 * currently selected target. Returns 0, or 1 with diagnostics on
 * stderr. `preprocess` runs the C preprocessor first, which is what
 * distinguishes `.S` from `.s`. */
int gas_assemble(const char *in_path, const char *out_path, int preprocess);

#endif
