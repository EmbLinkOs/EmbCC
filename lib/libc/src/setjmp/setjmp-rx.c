/* setjmp/longjmp for Renesas RX.
 *
 * The RX ABI calls r6-r13 callee-saved; r0 is the stack pointer, and a
 * call (bsr, jsr) pushes its return address there, which rts pops. So
 * setjmp records r6-r13, the return address on top of its stack, and the
 * stack pointer its caller has once that is popped; longjmp puts them
 * back and jumps to the address.
 *
 *   jmp_buf, in words: [0..7] = r6-r13, [8] = return address, [9] = sp
 *
 * The RX ABI puts an underscore before every C name, so these are
 * _setjmp and _longjmp in the object.
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4). */
#if defined(__RX__)
__asm__(
".text\n"
".globl _setjmp\n"
".type _setjmp, @function\n"
"_setjmp:\n"
"    mov.l r6, [r1]\n"
"    mov.l r7, 4[r1]\n"
"    mov.l r8, 8[r1]\n"
"    mov.l r9, 12[r1]\n"
"    mov.l r10, 16[r1]\n"
"    mov.l r11, 20[r1]\n"
"    mov.l r12, 24[r1]\n"
"    mov.l r13, 28[r1]\n"
"    mov.l [r0], r2\n"
"    mov.l r2, 32[r1]\n"
"    add #4, r0, r2\n"
"    mov.l r2, 36[r1]\n"
"    mov.l #0, r1\n"
"    rts\n"
".size _setjmp, .-_setjmp\n"
".globl _longjmp\n"
".type _longjmp, @function\n"
"_longjmp:\n"
"    mov.l [r1], r6\n"
"    mov.l 4[r1], r7\n"
"    mov.l 8[r1], r8\n"
"    mov.l 12[r1], r9\n"
"    mov.l 16[r1], r10\n"
"    mov.l 20[r1], r11\n"
"    mov.l 24[r1], r12\n"
"    mov.l 28[r1], r13\n"
"    mov.l 32[r1], r3\n"
"    mov.l 36[r1], r0\n"
"    mov.l r2, r1\n"
"    cmp #0, r1\n"
"    bne 1f\n"
"    mov.l #1, r1\n"
"1:  jmp r3\n"
".size _longjmp, .-_longjmp\n"
);
#endif
