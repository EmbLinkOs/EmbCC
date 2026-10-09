/* The ARMv7-M inline-asm assembler.
 *
 * Its vocabulary is what a Cortex-M program reaches inline assembly FOR
 * and cannot say in C: the special registers (mrs/msr), the interrupt
 * masks (cpsid/cpsie), the barriers, the wait hints, the exclusive pair
 * that an atomic is built from, and the handful of arithmetic and
 * memory instructions a hand-written sequence mixes in with them. That
 * is the CMSIS core set -- the one every ARM C project's headers open
 * with -- rather than a general Thumb-2 assembler.
 *
 * A statement outside it fails with a message naming the statement,
 * never with a guessed encoding (THE RULE). And every instruction is
 * encoded through src/arch/thumb/emit.c, the same encoders the code
 * generator uses and tools/thumbcheck round-trips through
 * llvm-objdump, so inline asm and compiled code cannot disagree.
 *
 * ONE THING THIS DOES THAT THE RISC-V ONE NEED NOT. Thumb-2 has 16-bit
 * and 32-bit encodings of the same mnemonic, and which one an assembler
 * picks changes the size of the output. The encoders already choose --
 * t_alu_imm returns 0 when a value will not fit the narrow form, and
 * the caller widens -- so this file makes no width decision of its own.
 * A `.w` suffix is accepted and ignored for that reason: it is a request
 * for a width the encoder has already worked out.
 *
 * Input is GNU ARM syntax with the operands ALREADY substituted (irgen
 * turns %0 and %[name] into register names first), so this sees plain
 * text such as "mrs r0, primask; cpsid i". Statements separate on ';'
 * and newlines; '@' and "//" start a comment.
 */
#ifndef EMBCC_ARCH_THUMB_ASM_H
#define EMBCC_ARCH_THUMB_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded template to `out`. Returns 0, or -1 with a
 * NUL-terminated message in err[0..errlen). */
int tasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* The register a name denotes -- r0..r15, and sp/lr/pc for r13/r14/r15
 * -- or -1. Used for `register T v __asm__("r0")` and clobber lists. */
int tasm_gpr(const char *name, int len);

/* The inverse, for irgen to substitute into a template. */
const char *t_reg_name(int reg);

/* An IT block outlives the statement that opens it: tasm_reset() forgets
 * one, and tasm_open() says whether the last block is still owed
 * instructions -- an error at the end of a template or a file. */
void tasm_reset(void);
int tasm_open(void);

/* For the file assembler: is `w` an operand word of this statement --
 * a special register, an interrupt mask, an IT condition, a barrier
 * option, a floating-point register -- rather than a symbol? */
int tasm_is_word(const char *stmt, const char *w, int wlen);

/* The statements whose operand is a symbol (`ldr rd, =sym`,
 * `movw rd, #:lower16:sym`), rewritten for relocation. */
struct asm_symform;
int tasm_symform(const char *stmt, struct asm_symform *f);

/* Writes one assembly line per vocabulary entry, so the golden test can
 * hand the SAME lines to llvm-mc and compare. Generated from the tables
 * themselves: an entry added here cannot escape the referee. */
void tasm_vocabulary(FILE *f);
/* ... and its data directives alone (.byte, .short, .word, .quad and the
 * other spellings), which every level takes: what tests/golden/thumb-asm.sh
 * also assembles for ARMv6-M and ARMv8-M Baseline */
void tasm_vocabulary_data(FILE *f);
/* ARMv8-M's: Mainline's additions to the above (base 0), or every 32-bit
 * instruction ARMv8-M Baseline has (base 1). */
void tasm_vocabulary_v8m(FILE *f, int base);

/* Branch relaxation for the file assembler: force the wide form for the
 * next statement, and ask whether a branch went wide on its own. */
void tasm_set_wide(int wide);
int tasm_took_wide(void);

/* The architecture level the next statements are for: 6 (ARMv6-M), 7
 * (ARMv7-M), 8 (ARMv8-M Mainline) or TASM_V8M_BASE. ARMv8-M adds the
 * stack-limit registers, the security extension and the acquire/release
 * forms; at the two Thumb-1 levels every 32-bit encoding a statement makes
 * is checked against what the core has. */
void tasm_set_arch(int level);
/* tasm_set_arch's value for ARMv8-M Baseline: the Thumb-1 level (6) with
 * Baseline's 32-bit instructions and the security extension. */
#define TASM_V8M_BASE 9
/* Whether the core has the DSP extension (ARMv7E-M; ARMv8-M Mainline
 * with it): sadd16, smlad, pkhbt and the rest are refused without it, as
 * llvm-mc refuses them. Off until a caller says otherwise. */
void tasm_set_dsp(int on);
/* The DSP extension's instructions, ssat/usat and the extends, across the
 * registers and immediates each field takes, for the referee
 * (tests/golden/thumb-dsp.sh): one line each. */
void tasm_vocabulary_dsp(FILE *f);
/* The multiplies, rev16/revsh, rrx, the bit fields and ldrd/strd (and in
 * ARM state the doubleword exclusives), the same way, for
 * tests/golden/arm-asm-more.sh. Both vocabularies are ARM state's when
 * t_isa_a32 is set. */
void tasm_vocabulary_more(FILE *f);

#endif
