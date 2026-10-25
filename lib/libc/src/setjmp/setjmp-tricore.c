/* setjmp/longjmp for TriCore (TC1.6), whose calls keep their own context.
 *
 * A `call` saves the caller's upper context -- PCXI, PSW, A10-A15,
 * D8-D15, sixteen words -- in a context save area (CSA) taken from the
 * free list FCX, and links it from PCXI; `ret` reloads it and gives the
 * area back. So the registers setjmp's caller keeps are exactly the
 * context its call to setjmp saved: setjmp copies that area, which PCXI
 * points at, into the buffer, with the address the call returns to.
 *
 * longjmp has to do what the returns it skips would have: give back every
 * context saved since -- each call's between setjmp's caller and here,
 * this one's included, and any svlcx's lower ones -- walking PCXI's chain
 * until it reaches the link setjmp's caller had, and pushing each area
 * onto FCX. Interrupts are off for the walk, since an interrupt takes
 * its own area from FCX. Then PCXI and PSW are the caller's again, its
 * A10-A15 and D8-D15 are reloaded from the copy, and longjmp jumps to
 * where setjmp returned, with the result in D2.
 *
 * A link's segment is bits 19:16 and its offset 15:0; the area is at
 * segment << 28 | offset << 6.
 *
 *   jmp_buf, in words: [0..15] = the upper context (PCXI, PSW, A10, A11,
 *                      D8-D11, A12-A15, D12-D15), [16] = return address
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4). */
/* The symbols' names: lib/rt/builtin-setjmp.c builds this file again as
 * __builtin_setjmp's and __builtin_longjmp's own pair. */
#ifndef SJ_SETJMP
#define SJ_SETJMP  "setjmp"
#define SJ_LONGJMP "longjmp"
#endif

#if defined(__TRICORE__) || defined(__tricore__)

/* d0's link -> a2, the area's address (through d1, d2) */
#define SJ_TC_AREA \
    "    sh d1, d0, -16\n"                                                \
    "    and d1, d1, 15\n"                                                \
    "    sh d1, d1, 28\n"                                                 \
    "    sh d2, d0, 16\n"                                                 \
    "    sh d2, d2, -10\n"                                                \
    "    or d1, d1, d2\n"                                                 \
    "    mov.a a2, d1\n"
#define SJ_TC_CP(o) "    ld.w d0, [a2]" #o "\n    st.w [a4]" #o ", d0\n"

__asm__(
".text\n"
".globl " SJ_SETJMP "\n"
".type " SJ_SETJMP ", @function\n"
 SJ_SETJMP ":\n"                         /* env in a4 */
"    mfcr d0, pcxi\n"
SJ_TC_AREA
SJ_TC_CP(0) SJ_TC_CP(4) SJ_TC_CP(8) SJ_TC_CP(12)
SJ_TC_CP(16) SJ_TC_CP(20) SJ_TC_CP(24) SJ_TC_CP(28)
SJ_TC_CP(32) SJ_TC_CP(36) SJ_TC_CP(40) SJ_TC_CP(44)
SJ_TC_CP(48) SJ_TC_CP(52) SJ_TC_CP(56) SJ_TC_CP(60)
"    st.a [a4]64, a11\n"
"    mov d2, 0\n"
"    ret\n"
".size " SJ_SETJMP ", .-" SJ_SETJMP "\n"

".globl " SJ_LONGJMP "\n"
".type " SJ_LONGJMP ", @function\n"
 SJ_LONGJMP ":\n"                        /* env in a4, val in d4 */
"    mfcr d3, 0xfe2c\n"             /* ICR, put back at the end */
"    disable\n"
"    ld.w d5, [a4]0\n"              /* the link to stop at */
"    mfcr d0, pcxi\n"
"1:  xor d6, d0, d5\n"
"    sh d6, d6, 12\n"               /* segment and offset only */
"    jz d6, 2f\n"
SJ_TC_AREA
"    ld.w d7, [a2]0\n"              /* the next one */
"    mfcr d6, fcx\n"
"    st.w [a2]0, d6\n"
"    sh d6, d0, 12\n"
"    sh d6, d6, -12\n"
"    dsync\n"
"    mtcr fcx, d6\n"                /* this one is free */
"    isync\n"
"    mov d0, d7\n"
"    j 1b\n"
"2:  ld.a a2, [a4]64\n"
"    dsync\n"
"    mtcr pcxi, d5\n"
"    isync\n"
"    ld.w d6, [a4]4\n"
"    mtcr psw, d6\n"
"    isync\n"
"    ld.a a10, [a4]8\n"
"    ld.a a11, [a4]12\n"
"    ld.w d8, [a4]16\n"
"    ld.w d9, [a4]20\n"
"    ld.w d10, [a4]24\n"
"    ld.w d11, [a4]28\n"
"    ld.a a12, [a4]32\n"
"    ld.a a13, [a4]36\n"
"    ld.a a14, [a4]40\n"
"    ld.a a15, [a4]44\n"
"    ld.w d12, [a4]48\n"
"    ld.w d13, [a4]52\n"
"    ld.w d14, [a4]56\n"
"    ld.w d15, [a4]60\n"
"    mtcr 0xfe2c, d3\n"
"    isync\n"
"    mov d2, d4\n"
"    jnz d2, 3f\n"
"    mov d2, 1\n"
"3:  ji a2\n"
".size " SJ_LONGJMP ", .-" SJ_LONGJMP "\n"
);

#endif
