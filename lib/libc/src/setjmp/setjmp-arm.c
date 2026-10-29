/* setjmp/longjmp for ARMv7-A in ARM state (armv7a-none-eabi, AAPCS).
 *
 * AAPCS32 calls r4-r11 callee-saved, with sp and the return address, and
 * with VFP d8-d15 too: the hard-float build (armv7a-none-eabihf) saves
 * them, as s16-s31. Written in assembly the file assembler takes in ARM
 * state, as the x86-64 and aarch64 files are in theirs -- no C
 * expression denotes the registers. Thumb is setjmp-thumb.c.
 *
 * sp goes through r12 rather than into the register list: an STM or LDM
 * naming sp is deprecated in ARM state and does not exist in Thumb.
 *
 *   jmp_buf, in words: [0..7] = r4-r11, [8] = sp, [9] = lr,
 *                      [10..25] = s16-s31
 */
/* The symbols' names: lib/rt/builtin-setjmp.c builds this file again as
 * __builtin_setjmp's and __builtin_longjmp's own pair. */
#ifndef SJ_SETJMP
#define SJ_SETJMP  "setjmp"
#define SJ_LONGJMP "longjmp"
#endif

#if defined(__arm__) && !defined(__thumb__)

__asm__(
".globl " SJ_SETJMP "\n"
".type " SJ_SETJMP ", %function\n"
 SJ_SETJMP ":\n"
"    mov r12, sp\n"
"    stmia r0, {r4-r12, lr}\n"
#if defined(__ARM_FP)
"    add r1, r0, #40\n"
"    vstmia r1, {s16-s31}\n"
#endif
"    mov r0, #0\n"
"    bx lr\n"
".size " SJ_SETJMP ", .-" SJ_SETJMP "\n"

".globl " SJ_LONGJMP "\n"
".type " SJ_LONGJMP ", %function\n"
 SJ_LONGJMP ":\n"
#if defined(__ARM_FP)
"    add r2, r0, #40\n"
"    vldmia r2, {s16-s31}\n"
#endif
"    ldmia r0, {r4-r12, lr}\n"
"    mov sp, r12\n"
/* longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4) */
"    movs r0, r1\n"
"    moveq r0, #1\n"
"    bx lr\n"
".size " SJ_LONGJMP ", .-" SJ_LONGJMP "\n"
);

#endif
