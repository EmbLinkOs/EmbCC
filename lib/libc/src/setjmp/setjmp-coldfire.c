/* setjmp/longjmp for ColdFire (and m68k), the SVR4 convention.
 *
 * d2-d7 and a2-a6 are callee-saved, with sp (a7); the arguments come on
 * the stack and the result goes in d0. setjmp records the registers with
 * one movem, the return address on top of its stack and the stack
 * pointer there; longjmp puts the registers back, the stack pointer
 * where it is once that address is popped, and jumps to it. EmbCC's
 * ColdFire is soft-float, so no FP register is saved.
 *
 *   jmp_buf, in words: [0..5] = d2-d7, [6..10] = a2-a6,
 *                      [11] = return address, [12] = sp at the call
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4). */
#if defined(__mcoldfire__) || defined(__m68k__)
__asm__(
".text\n"
".globl setjmp\n"
".type setjmp, @function\n"
"setjmp:\n"
"    move.l 4(%sp), %a0\n"
"    movem.l %d2-%d7/%a2-%a6, (%a0)\n"
"    move.l (%sp), %d0\n"
"    move.l %d0, 44(%a0)\n"
"    move.l %sp, 48(%a0)\n"
"    clr.l %d0\n"
"    rts\n"
".size setjmp, .-setjmp\n"
".globl longjmp\n"
".type longjmp, @function\n"
"longjmp:\n"
"    move.l 4(%sp), %a0\n"
"    move.l 8(%sp), %d0\n"
"    movem.l (%a0), %d2-%d7/%a2-%a6\n"
"    move.l 44(%a0), %a1\n"
"    move.l 48(%a0), %sp\n"
"    addq.l #4, %sp\n"
"    tst.l %d0\n"
"    bne 1f\n"
"    moveq #1, %d0\n"
"1:  jmp (%a1)\n"
".size longjmp, .-longjmp\n"
);
#endif
