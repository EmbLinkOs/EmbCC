/* The harness's output routines, for the HOST.
 *
 * tests/golden/thumb-exec.sh compiles a 64-bit test program twice: once
 * for ARMv7-M, where io.c writes to the board's UART, and once for the
 * machine the suite runs on, where this writes to stdout. The program
 * itself is the same text, so whatever the host computes is what the
 * target must compute -- `long long` arithmetic has one answer.
 *
 * That makes the host compiler the reference for this program, which is
 * what is wanted: clang for thumbv7m emits __aeabi_ldivmod for a 64-bit
 * divide and EmbCC emits __divdi3, so the two cannot share a runtime
 * and a cross-compiled reference would need a helper nothing here can
 * write (its quotient and remainder come back in four registers at
 * once, which C cannot express).
 */
#include <stdio.h>

void writec(int c) { putchar(c); }
void puts_(const char *s) { while (*s) putchar(*s++); }
void putn(long v) { printf("%ld ", v); }
