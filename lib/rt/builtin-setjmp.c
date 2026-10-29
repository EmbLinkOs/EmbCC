/* __builtin_setjmp and __builtin_longjmp's halves, for the targets whose
 * C library is EmbCC's (and so for a program linked with none at all).
 *
 * GCC's builtins save into a five-word buffer of the program's, and need
 * nothing linked: its code saves the frame and stack pointers and a
 * resume label, and has every other register restored by the function
 * that called __builtin_setjmp. EmbCC lowers them onto an ordinary
 * setjmp instead (src/ir/irgen.c): __builtin_setjmp(buf) keeps a jmp_buf
 * in its caller's own frame, stores that jmp_buf's address in buf[0] and
 * calls __embcc_setjmp on it; __builtin_longjmp(buf, 1) calls
 * __embcc_longjmp(buf[0], 1). The frame is still there when the longjmp
 * comes -- GCC's rule for the pair is that the setjmp's caller has not
 * returned -- and the five words hold one pointer.
 *
 * These are lib/libc's setjmp and longjmp, built again under names of
 * the compiler's own (src/setjmp/'s SJ_SETJMP), so a program that defines
 * its own setjmp, or links no C library, still has them. Darwin's and
 * glibc's builds call the system's _setjmp and _longjmp, and need none of
 * this. */
#define SJ_SETJMP  "__embcc_setjmp"
#define SJ_LONGJMP "__embcc_longjmp"

#if defined(__AVR__)
#include "../avr/setjmp.c"
#elif defined(__x86_64__) && !defined(__APPLE__) && !defined(_WIN32)
#include "../libc/src/setjmp/setjmp-x86_64.c"
#elif defined(__aarch64__) && !defined(__APPLE__)
#include "../libc/src/setjmp/setjmp-aarch64.c"
#elif defined(__arm__) || defined(__thumb__)
#include "../libc/src/setjmp/setjmp-arm.c"
#include "../libc/src/setjmp/setjmp-thumb.c"
#elif defined(__riscv)
#include "../libc/src/setjmp/setjmp-riscv.c"
#elif defined(__mips__)
#include "../libc/src/setjmp/setjmp-mips.c"
#elif defined(__loongarch__)
#include "../libc/src/setjmp/setjmp-loongarch.c"
#elif defined(__tricore__)
#include "../libc/src/setjmp/setjmp-tricore.c"
#elif defined(__XTENSA__) || defined(__xtensa__)
#include "../libc/src/setjmp/setjmp-xtensa.c"
#elif defined(__PPC__) || defined(__powerpc__)
#include "../libc/src/setjmp/setjmp-ppc.c"
#elif defined(__RX__)
#include "../libc/src/setjmp/setjmp-rx.c"
#elif defined(__sparc__)
#include "../libc/src/setjmp/setjmp-sparc.c"
#elif defined(__mcoldfire__) || defined(__m68k__)
#include "../libc/src/setjmp/setjmp-coldfire.c"
#endif
