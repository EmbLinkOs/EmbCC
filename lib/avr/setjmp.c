/* setjmp/longjmp for the AVR: avr5 and avr51 (a 2-byte PC: the ATmega328P's
 * family, the ATmega1280), and avr6 (a 3-byte PC: the ATmega2560).
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
 *                      [20] = SREG, [21..22] = the return address,
 *                      [23] = its bits 16-21 on avr6
 *
 * On avr6 a call pushes a 3-byte PC, bits 16-21 at the lowest address:
 * setjmp pops that byte first, and longjmp returns with eijmp, EIND set
 * from it, so a caller past 128 KiB of flash is returned to as well.
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4): val + carry
 * of val < 1, unsigned. */
/* The symbols' names: lib/rt/builtin-setjmp.c builds this file again as
 * __builtin_setjmp's and __builtin_longjmp's own pair. */
#ifndef SJ_SETJMP
#define SJ_SETJMP  "setjmp"
#define SJ_LONGJMP "longjmp"
#endif

#if defined(__AVR__)

#if !defined(__AVR_2_BYTE_PC__) && !defined(__AVR_3_BYTE_PC__)
#error "lib/avr's setjmp knows a 2-byte and a 3-byte PC"
#endif
#if defined(__AVR_3_BYTE_PC__)
#define SJ_POP_EXT "    pop r21\n"        /* bits 16-21, at the lowest address */
#define SJ_ST_EXT  "    st X+, r21\n"
#define SJ_RET     "    out 0x3c, r21\n    eijmp\n"   /* EIND */
#define SJ_LD_EXT  "    ld r21, X+\n"
#else
#define SJ_POP_EXT ""
#define SJ_ST_EXT  ""
#define SJ_RET     "    ijmp\n"
#define SJ_LD_EXT  ""
#endif

#define SJ_AVR_REGS(op) \
    op(2) op(3) op(4) op(5) op(6) op(7) op(8) op(9) op(10) op(11)         \
    op(12) op(13) op(14) op(15) op(16) op(17) op(28) op(29)
#define SJ_AVR_ST(r) "    st X+, r" #r "\n"
#define SJ_AVR_LD(r) "    ld r" #r ", X+\n"

__asm__(
".text\n"
".globl " SJ_SETJMP "\n"
".type " SJ_SETJMP ", @function\n"
 SJ_SETJMP ":\n"
"    movw r26, r24\n"               /* X = env */
SJ_AVR_REGS(SJ_AVR_ST)
SJ_POP_EXT
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
SJ_ST_EXT
"    clr r24\n"
"    clr r25\n"
SJ_RET
".size " SJ_SETJMP ", .-" SJ_SETJMP "\n"

".globl " SJ_LONGJMP "\n"
".type " SJ_LONGJMP ", @function\n"
 SJ_LONGJMP ":\n"
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
SJ_LD_EXT
SJ_RET
".size " SJ_LONGJMP ", .-" SJ_LONGJMP "\n"
);

#endif
