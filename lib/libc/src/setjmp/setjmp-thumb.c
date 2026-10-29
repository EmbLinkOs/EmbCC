/* setjmp/longjmp for Thumb, AAPCS: every Cortex-M.
 *
 * AAPCS calls r4-r11 callee-saved, with sp and the return address in lr,
 * and with a floating-point unit s16-s31 too (d8-d15). The single-
 * precision list is the one every FPU takes: FPv4-SP and FPv5's D16
 * alike (Cortex-M4F, M33F, M7). sp goes through a low register: no
 * ldm/stm list may name it.
 *
 *   jmp_buf, in words: [0..7] = r4-r11, [8] = sp, [9] = lr,
 *                      [10..25] = s16-s31
 *
 * Two bodies, chosen by the instruction set rather than written twice:
 * Thumb-2 (ARMv7-M, ARMv8-M Mainline) stores r4-r12 and lr in one stm;
 * ARMv6-M and ARMv8-M Baseline have only the 16-bit ldm/stm, which take
 * r0-r7, so r8-r11, sp and lr go through r1-r3 three at a time.
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4). */
/* The symbols' names: lib/rt/builtin-setjmp.c builds this file again as
 * __builtin_setjmp's and __builtin_longjmp's own pair. */
#ifndef SJ_SETJMP
#define SJ_SETJMP  "setjmp"
#define SJ_LONGJMP "longjmp"
#endif

#if defined(__thumb__)

#if defined(__thumb2__)
__asm__(
".text\n"
".globl " SJ_SETJMP "\n"
".type " SJ_SETJMP ", %function\n"
".thumb_func\n"
 SJ_SETJMP ":\n"
"    mov r12, sp\n"
"    stmia r0, {r4-r12, lr}\n"
#if defined(__ARM_FP)
"    add r1, r0, #40\n"
"    vstmia r1, {s16-s31}\n"
#endif
"    movs r0, #0\n"
"    bx lr\n"
".size " SJ_SETJMP ", .-" SJ_SETJMP "\n"

".globl " SJ_LONGJMP "\n"
".type " SJ_LONGJMP ", %function\n"
".thumb_func\n"
 SJ_LONGJMP ":\n"
#if defined(__ARM_FP)
"    add r2, r0, #40\n"
"    vldmia r2, {s16-s31}\n"
#endif
"    ldmia r0, {r4-r12, lr}\n"
"    mov sp, r12\n"
"    movs r0, r1\n"
"    bne 1f\n"
"    movs r0, #1\n"
"1:  bx lr\n"
".size " SJ_LONGJMP ", .-" SJ_LONGJMP "\n"
);
#else
__asm__(
".text\n"
".globl " SJ_SETJMP "\n"
".type " SJ_SETJMP ", %function\n"
".thumb_func\n"
 SJ_SETJMP ":\n"
"    stmia r0!, {r4-r7}\n"
"    mov r1, r8\n"
"    mov r2, r9\n"
"    mov r3, r10\n"
"    stmia r0!, {r1-r3}\n"
"    mov r1, r11\n"
"    mov r2, sp\n"
"    mov r3, lr\n"
"    stmia r0!, {r1-r3}\n"
"    movs r0, #0\n"
"    bx lr\n"
".size " SJ_SETJMP ", .-" SJ_SETJMP "\n"

".globl " SJ_LONGJMP "\n"
".type " SJ_LONGJMP ", %function\n"
".thumb_func\n"
 SJ_LONGJMP ":\n"
"    adds r0, #16\n"              /* r8 onwards, through r2-r7 */
"    ldmia r0!, {r2-r7}\n"
"    mov r8, r2\n"
"    mov r9, r3\n"
"    mov r10, r4\n"
"    mov r11, r5\n"
"    mov sp, r6\n"
"    mov lr, r7\n"
"    subs r0, #40\n"              /* and then r4-r7 themselves */
"    ldmia r0!, {r4-r7}\n"
"    movs r0, r1\n"
"    bne 1f\n"
"    movs r0, #1\n"
"1:  bx lr\n"
".size " SJ_LONGJMP ", .-" SJ_LONGJMP "\n"
);
#endif

#endif
