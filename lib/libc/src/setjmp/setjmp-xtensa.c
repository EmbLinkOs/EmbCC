/* setjmp/longjmp for Xtensa, the windowed ABI (ESP32, ESP32-S3).
 *
 * A call8 leaves the caller's a0-a7 to it and gives a8-a15 away, and the
 * window exceptions spill a frame's registers to its stack and refill
 * them on the way back. Neither function here executes `entry`, so each
 * runs in its caller's window: a call8 sets PS.CALLINC and the return
 * address in a8, but only entry rotates the window. That is how setjmp
 * sees its caller's own a0-a7 -- the registers the caller keeps across
 * the call -- and records them, with the return address; it answers in
 * a10, where the caller reads a call8's result, and goes back by jx.
 *
 * longjmp first spills every window but its own. A chain of calls, six
 * deep, each touching a15, makes the window-overflow handler save every
 * live frame behind it -- 72 registers, more than an LX6 or LX7 has, and
 * harmless to go round twice on a 32-register core. Its first call is a
 * call8, as the caller's frame was laid out for (a call12 caller's spill
 * takes 16 bytes more of its frame), and the rest are call12s. Then
 * whatever the caller of setjmp's caller and the frames above it held is
 * on their stacks, where the underflow handler will look for it, and no
 * window behind this one is live. Then longjmp becomes setjmp's caller: its
 * a0-a7 from the buffer, the result in a10, and a jump to where setjmp
 * returned. No privileged instruction, and nothing of the system's: the
 * window vectors any windowed program already has. Callers use call8,
 * as EmbCC and GCC do.
 *
 * A windowed return address has the call's increment in its top two
 * bits; the real ones are those of the code calling, which is in the
 * same 1 GB as this (a windowed call cannot leave it), so they come from
 * this file's own address in a literal.
 *
 *   jmp_buf, in words: [0..7] = a0-a7, [8] = the return address
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4). EmbCC's Xtensa
 * is soft-float. */
#if defined(__XTENSA__) || defined(__xtensa__)

__asm__(
".text\n"
".align 4\n"
".Lsj_here:\n"
"    .long setjmp\n"

".globl setjmp\n"
".type setjmp, @function\n"
".align 4\n"
"setjmp:\n"                         /* the caller's window: env in a10 */
"    s32i a0, a10, 0\n"
"    s32i a1, a10, 4\n"
"    s32i a2, a10, 8\n"
"    s32i a3, a10, 12\n"
"    s32i a4, a10, 16\n"
"    s32i a5, a10, 20\n"
"    s32i a6, a10, 24\n"
"    s32i a7, a10, 28\n"
"    s32i a8, a10, 32\n"
"    l32r a9, .Lsj_here\n"
"    extui a9, a9, 30, 2\n"
"    slli a9, a9, 30\n"
"    slli a8, a8, 2\n"
"    srli a8, a8, 2\n"
"    or a8, a8, a9\n"
"    movi a10, 0\n"
"    jx a8\n"
".size setjmp, .-setjmp\n"

".globl longjmp\n"
".type longjmp, @function\n"
".align 4\n"
"longjmp:\n"                        /* env in a10, val in a11 */
"    mov a2, a10\n"                 /* the caller's a2-a7 survive a call8 */
"    mov a3, a11\n"
"    call8 .Lsj_spill1\n"
"    l32i a8, a2, 32\n"
"    l32r a9, .Lsj_here\n"
"    extui a9, a9, 30, 2\n"
"    slli a9, a9, 30\n"
"    slli a8, a8, 2\n"
"    srli a8, a8, 2\n"
"    or a8, a8, a9\n"
"    movi a10, 1\n"
"    movnez a10, a3, a3\n"
"    l32i a0, a2, 0\n"
"    l32i a1, a2, 4\n"
"    l32i a4, a2, 16\n"
"    l32i a5, a2, 20\n"
"    l32i a6, a2, 24\n"
"    l32i a7, a2, 28\n"
"    l32i a3, a2, 12\n"
"    l32i a2, a2, 8\n"               /* the base, last */
"    jx a8\n"
".size longjmp, .-longjmp\n"

/* The spill: each level's frame holds its own extra save area (a call12
 * caller's is 32 bytes) under the base save area above it. The first is
 * entered by a call8 and the others by a call12, which reach 2 + 3 * 5 +
 * 3 = 20 windows ahead: all of 64 registers and its own frame again. */
".align 4\n"
".Lsj_spill1:\n"
"    entry a1, 48\n"
"    mov a15, a15\n"
"    call12 .Lsj_spill2\n"
"    retw\n"
".align 4\n"
".Lsj_spill2:\n"
"    entry a1, 48\n"
"    mov a15, a15\n"
"    call12 .Lsj_spill3\n"
"    retw\n"
".align 4\n"
".Lsj_spill3:\n"
"    entry a1, 48\n"
"    mov a15, a15\n"
"    call12 .Lsj_spill4\n"
"    retw\n"
".align 4\n"
".Lsj_spill4:\n"
"    entry a1, 48\n"
"    mov a15, a15\n"
"    call12 .Lsj_spill5\n"
"    retw\n"
".align 4\n"
".Lsj_spill5:\n"
"    entry a1, 48\n"
"    mov a15, a15\n"
"    call12 .Lsj_spill6\n"
"    retw\n"
".align 4\n"
".Lsj_spill6:\n"
"    entry a1, 48\n"
"    mov a15, a15\n"
"    retw\n"
);

#endif
