/* The harness's output routines, for the HOST.
 *
 * tests/golden/riscv-exec.sh compiles a test program twice: once for
 * RISC-V, where io.c writes to the board's UART, and once for the
 * machine the suite runs on, where this writes to stdout. The program
 * itself is the same text, so whatever the host computes is what the
 * target must compute -- integer and IEEE arithmetic each have one
 * answer, whatever the register width.
 *
 * The host is the reference rather than a cross-compiled clang for two
 * of the four programs, and deliberately: a 64-bit divide at RV32 goes
 * through EmbCC's own __divdi3, and comparing against a clang image that
 * uses its libgcc would compare two runtimes rather than two compilers.
 * Where the comparison IS against clang, riscv-exec.sh says so.
 */
#include <stdio.h>

void writec(int c) { putchar(c); }
void puts_(const char *s) { while (*s) putchar(*s++); }
void putn(long v) { printf("%ld ", v); }
