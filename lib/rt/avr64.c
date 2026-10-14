/* The AVR compiler runtime, sixty-four bits: multiply, divide and remainder.
 *
 * A SEPARATE object from lib/rt/avr.c on purpose. A runtime library is one
 * routine per object so that a program pays only for what it calls, and this
 * target makes that concrete: with the 64-bit helpers in the same file, an
 * ATmega328P program that never writes `long long` still carried them, and
 * at -O0 that took a test image to 34718 bytes on a part with 32768 of
 * flash. It failed as `CALL 0x8770` -- a call past the end of memory.
 *
 * The backend does eight-byte add, subtract, shift, compare and branch
 * itself, a byte at a time through memory: `ldd` and `std` do not touch
 * SREG on this machine, so the carry survives the loads that the next byte
 * needs. Multiply and divide are calls, for the same reason the four-byte
 * ones are -- AVR's `mul` is 8x8 into r1:r0 and destroys the zero register,
 * and there is no divide instruction at all.
 *
 * Same constraint as every other file here (rt.h): no routine may use the
 * operation it implements. These are shift-and-add and restoring division,
 * and every shift is a constant one or an `x += x`.
 *
 * The names are libgcc's, so an object of ours links beside one of theirs.
 */
#ifdef __AVR__

typedef unsigned long long u64_;
typedef long long          s64_;

s64_ __muldi3(s64_ a, s64_ b)
{
    u64_ x = (u64_)a, y = (u64_)b, r = 0;

    while (y) {
        if (y & 1)
            r += x;
        x += x;
        y >>= 1;
    }
    return (s64_)r;
}

/* Division, in assembly: restoring division, one quotient bit per step, the
 * dividend shifting into the remainder as the quotient shifts in behind it.
 * The C version of the same loop moved eight-byte values through memory a
 * byte at a time and took some 23000 cycles a division -- 1.4 ms at 16 MHz,
 * which an RTOS converting a timeout pays on every call; this is about 2500.
 *
 * libgcc's interface and avr-gcc's convention: the dividend in r18-r25, the
 * divisor in r10-r17 (call-saved, so read and left alone), the result in
 * r18-r25. The remainder is built in r2-r9, saved and restored here; it
 * cannot carry out of r9, being below 2^k after the k-th of 64 steps.
 * Dividing by 0 gives all ones and the dividend as the remainder, as the C
 * version did. */
u64_ __udivdi3(u64_ a, u64_ b);
u64_ __umoddi3(u64_ a, u64_ b);

__asm__(
    "    .globl  __udivdi3\n"
    "__udivdi3:\n"
    "    push r2\n    push r3\n    push r4\n    push r5\n"
    "    push r6\n    push r7\n    push r8\n    push r9\n"
    "    rcall   __emb_udivmod64\n"
    "    rjmp    __emb_udivmod64_out\n"
    "    .globl  __umoddi3\n"
    "__umoddi3:\n"
    "    push r2\n    push r3\n    push r4\n    push r5\n"
    "    push r6\n    push r7\n    push r8\n    push r9\n"
    "    rcall   __emb_udivmod64\n"
    "    movw    r18, r2\n"
    "    movw    r20, r4\n"
    "    movw    r22, r6\n"
    "    movw    r24, r8\n"
    "__emb_udivmod64_out:\n"
    "    pop r9\n    pop r8\n    pop r7\n    pop r6\n"
    "    pop r5\n    pop r4\n    pop r3\n    pop r2\n"
    "    ret\n"
    "__emb_udivmod64:\n"
    "    clr     r2\n"
    "    clr     r3\n"
    "    movw    r4, r2\n"
    "    movw    r6, r2\n"
    "    movw    r8, r2\n"
    "    ldi     r26, 64\n"
    "1:\n"
    "    lsl r18\n    rol r19\n    rol r20\n    rol r21\n"
    "    rol r22\n    rol r23\n    rol r24\n    rol r25\n"
    "    rol r2\n    rol r3\n    rol r4\n    rol r5\n"
    "    rol r6\n    rol r7\n    rol r8\n    rol r9\n"
    "    cp r2, r10\n    cpc r3, r11\n    cpc r4, r12\n    cpc r5, r13\n"
    "    cpc r6, r14\n    cpc r7, r15\n    cpc r8, r16\n    cpc r9, r17\n"
    "    brcs    3f\n"
    "    sub r2, r10\n    sbc r3, r11\n    sbc r4, r12\n    sbc r5, r13\n"
    "    sbc r6, r14\n    sbc r7, r15\n    sbc r8, r16\n    sbc r9, r17\n"
    "    ori     r18, 1\n"
    "3:\n"
    "    dec     r26\n"
    "    brne    1b\n"
    "    ret\n");

static u64_ mag64(s64_ v, int *neg)
{
    if (v < 0) {
        *neg = !*neg;
        return -(u64_)v;
    }
    return (u64_)v;
}

s64_ __divdi3(s64_ a, s64_ b)
{
    int neg = 0;
    u64_ ua = mag64(a, &neg), ub = mag64(b, &neg);
    u64_ q = __udivdi3(ua, ub);
    return neg ? -(s64_)q : (s64_)q;
}

s64_ __moddi3(s64_ a, s64_ b)
{
    int dneg = 0, bneg = 0;
    u64_ ua = mag64(a, &dneg), ub = mag64(b, &bneg);
    u64_ r = __umoddi3(ua, ub);
    return dneg ? -(s64_)r : (s64_)r;    /* the DIVIDEND's sign */
}


#else

/* Not this machine. lib/rt is built by a wildcard for every target, and a
 * translation unit has to contain at least one declaration. */
typedef int embcc_rt_avr64_is_not_this_target;

#endif /* __AVR__ */
