/* setjmp/longjmp for the AVR (avr5, a 2-byte PC: the ATmega328P's family).
 *
 * The AVR GCC ABI, which EmbCC's AVR backend keeps: r2-r17 and Y
 * (r28:r29) are callee-saved, r1 is the zero register and r0 the scratch;
 * the pointer argument is in r25:r24, the second in r23:r22, and an int
 * comes back in r25:r24. The return address is on the stack, high byte
 * first. setjmp pops it, so the stack pointer it records is the one its
 * caller has after the call, and jumps back through Z; longjmp puts the
 * stack pointer back with interrupts held off between its two halves --
 * an `out` to SREG takes effect after the instruction that follows it,
 * so SPL is written before an interrupt can see a half-moved SP -- and
 * SREG's I flag as setjmp found it.
 *
 *   jmp_buf, in bytes: [0..15] = r2-r17, [16..17] = Y, [18..19] = SP,
 *                      [20] = SREG, [21..22] = the return address
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4): val + carry
 * of val < 1, unsigned. */
#if defined(__AVR__)

#if !defined(__AVR_2_BYTE_PC__)
#error "lib/avr's setjmp knows the 2-byte PC of an avr5 part"
#endif

#define SJ_AVR_REGS(op) \
    op(2) op(3) op(4) op(5) op(6) op(7) op(8) op(9) op(10) op(11)         \
    op(12) op(13) op(14) op(15) op(16) op(17) op(28) op(29)
#define SJ_AVR_ST(r) "    st X+, r" #r "\n"
#define SJ_AVR_LD(r) "    ld r" #r ", X+\n"

__asm__(
".text\n"
".globl setjmp\n"
".type setjmp, @function\n"
"setjmp:\n"
"    movw r26, r24\n"               /* X = env */
SJ_AVR_REGS(SJ_AVR_ST)
"    pop r31\n"                     /* Z = where to go back to */
"    pop r30\n"
"    in r24, 0x3d\n"                /* SPL */
"    st X+, r24\n"
"    in r24, 0x3e\n"                /* SPH */
"    st X+, r24\n"
"    in r24, 0x3f\n"                /* SREG */
"    st X+, r24\n"
"    st X+, r30\n"
"    st X+, r31\n"
"    clr r24\n"
"    clr r25\n"
"    ijmp\n"
".size setjmp, .-setjmp\n"

".globl longjmp\n"
".type longjmp, @function\n"
"longjmp:\n"
"    movw r26, r24\n"               /* X = env */
"    movw r24, r22\n"               /* the result: val, or 1 for 0 */
"    cpi r24, 1\n"
"    cpc r25, r1\n"
"    adc r24, r1\n"
SJ_AVR_REGS(SJ_AVR_LD)
"    ld r30, X+\n"
"    ld r31, X+\n"
"    ld r0, X+\n"
"    cli\n"
"    out 0x3e, r31\n"
"    out 0x3f, r0\n"
"    out 0x3d, r30\n"
"    ld r30, X+\n"
"    ld r31, X+\n"
"    ijmp\n"
".size longjmp, .-longjmp\n"
);

#endif
