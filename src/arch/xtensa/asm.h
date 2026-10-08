/* The Xtensa assembler: inline `__asm__` templates, and the statements of
 * a .s/.S file, a file-scope asm block or a naked function through
 * src/as/gas.c.
 *
 * The vocabulary is what ESP32 code writes -- FreeRTOS's xtensa port
 * (portasm.S, xtensa_vectors.S, xtensa_context.S, the rsil/wsr/rsr/isync
 * of portmacro.h) is the corpus it was measured on -- and every
 * instruction src/arch/xtensa/emit.c encodes: the core ALU, shifts and
 * SAR, loads and stores, movi/mov/addi/addmi, every branch form, j, jx,
 * the windowed and call0 calls, entry/retw/movsp/rotw, the zero-overhead
 * loops, rsr/wsr/xsr by the ESP32's special-register names or by number,
 * rur/wur THREADPTR, rsil/waiti, the syncs, the exception returns and the
 * conditional-store and window-handler loads and stores. A form outside
 * that list fails with a message naming it (THE RULE); the density
 * option's 16-bit `.n` forms are refused by name too, since nothing here
 * emits one. GNU's `_` prefix (no transformation) is accepted on every
 * instruction, and `bbci.l`/`bbsi.l` are bbci/bbsi on this little-endian
 * core.
 *
 * Every instruction goes through emit.c's encoders, the ones the code
 * generator uses; tests/golden/xtensa-asm.sh referees the whole vocabulary
 * with QEMU's de212 disassembler and, where it is installed, Espressif's
 * GNU as.
 *
 * The input is GNU Xtensa syntax with the operands substituted: registers
 * a0-a15 (`sp` is a1), constants as C expressions, and a branch, jump,
 * call, loop or l32r target as `.+N` / `.-N` -- the byte distance from
 * this instruction, which is where GNU's `.` is. A call's and an l32r's
 * target is a word address computed from the instruction's own address
 * rounded, so those two need to know where the instruction is
 * (xtasm_set_pc); the file assembler says, and inline asm, which cannot
 * know, refuses them. Inside a template numeric labels (`1:`, `1b`,
 * `1f`) work as in GCC's; any other label is the file assembler's.
 * Statements separate on ';' and newlines, and '#' and "//" start a
 * comment.
 */
#ifndef EMBCC_ARCH_XTENSA_ASM_H
#define EMBCC_ARCH_XTENSA_ASM_H

#include <stdio.h>

#include "../code.h"

/* Appends the encoded template to `out`. Returns 0, or -1 with a message
 * in err[0..errlen). */
int xtasm_assemble(const char *text, struct code *out, char *err, int errlen);

/* Where the next statement xtasm_assemble sees starts, in bytes from the
 * start of its section (4-aligned), for call and l32r targets; -1 when
 * that is not known. Forgotten after each xtasm_assemble. */
void xtasm_set_pc(long pc);

/* The address register a name denotes -- "a0".."a15", or "sp" -- or -1. */
int xtasm_gpr(const char *name, int len);

/* For src/as/gas.c: a register (so the driver does not take `a2` for a
 * symbol), a special- or user-register name where rsr/wsr/xsr/rur/wur
 * take one, and the statements whose target is a SYMBOL, with the
 * R_XTENSA_SLOT0_OP each carries. */
int xtasm_is_reg(const char *name, int len);
int xtasm_is_word(const char *stmt, const char *w, int len);
int xtasm_symform(const char *stmt, struct asm_symform *f);

/* Is `stmt` `movi aN, X` -- the form GNU as turns into an l32r of a
 * literal when X is a symbol or does not fit movi's 12 bits? Returns the
 * register, with the operand's text in *x, *xlen; -1 when it is not. */
int xtasm_movi_operand(const char *stmt, const char **x, int *xlen);

/* Does the statement begin with `entry`? (A literal pool is placed
 * before a function's entry, as GNU as's --text-section-literals has
 * it.) */
int xtasm_is_entry(const char *stmt);

/* The special register (0..255) a name denotes, and whether rsr (1),
 * wsr (2) and xsr (4) may name it on the ESP32; -1 when it is none. */
int xtasm_sr(const char *name, int len, int *access);

/* One line per vocabulary entry, for the referee: the statement, an @,
 * and the line QEMU's de212 disassembler prints for it (see
 * tools/xtasmcheck). */
void xtasm_vocabulary(FILE *f);

#endif
