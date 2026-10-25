/* setjmp/longjmp for ARMv7-A in ARM state (armv7a-none-eabi, AAPCS).
 *
 * AAPCS32 calls r4-r11 callee-saved, with sp and the return address;
 * this library's ARM build is soft-float, so no VFP register needs saving
 * (a hard-float build would add d8-d15). Written in assembly the file
 * assembler takes in ARM state, as the x86-64 and aarch64 files are in
 * theirs -- no C expression denotes the registers.
 *
 * sp goes through r12 rather than into the register list: an STM or LDM
 * naming sp is deprecated in ARM state and does not exist in Thumb.
 *
 *   jmp_buf: [0..7] = r4-r11, [8] = sp, [9] = lr
 */
#if defined(__arm__) && !defined(__thumb__)

__asm__(
".globl setjmp\n"
".type setjmp, %function\n"
"setjmp:\n"
"    mov r12, sp\n"
"    stmia r0, {r4-r12, lr}\n"
"    mov r0, #0\n"
"    bx lr\n"
".size setjmp, .-setjmp\n"

".globl longjmp\n"
".type longjmp, %function\n"
"longjmp:\n"
"    ldmia r0, {r4-r12, lr}\n"
"    mov sp, r12\n"
/* longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4) */
"    movs r0, r1\n"
"    moveq r0, #1\n"
"    bx lr\n"
".size longjmp, .-longjmp\n"
);

#endif
