/* setjmp/longjmp for MIPS: o32 (MIPS32, PIC32) and n64 (MIPS64), either
 * byte order.
 *
 * Both ABIs call s0-s7 ($16-$23), gp ($28), sp ($29), fp/s8 ($30) and ra
 * ($31) callee-saved; EmbCC's MIPS ABIs are soft-float, so no FP
 * register is. One body: the word size picks sw/lw or sd/ld and the slot
 * width.
 *
 *   jmp_buf: s0-s7, fp, sp, gp, ra
 *
 * Under `.set noreorder` the delay slots are written out: setjmp's
 * return carries its `move $2, $0`, and longjmp computes val + (val == 0)
 * -- longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4) -- without
 * movn, which MIPS32r6 removed. */
/* The symbols' names: lib/rt/builtin-setjmp.c builds this file again as
 * __builtin_setjmp's and __builtin_longjmp's own pair. */
#ifndef SJ_SETJMP
#define SJ_SETJMP  "setjmp"
#define SJ_LONGJMP "longjmp"
#endif

#if defined(__mips__)

#if _MIPS_SIM == _ABI64
#define SJ_S "sd"
#define SJ_L "ld"
#define SJ_X 8
#else
#define SJ_S "sw"
#define SJ_L "lw"
#define SJ_X 4
#endif
#define SJ_STR_(x) #x
#define SJ_STR(x)  SJ_STR_(x)
#define SJ_R(op, r, k) \
    "    " op " $" #r ", " SJ_STR(k) "*" SJ_STR(SJ_X) "($4)\n"
#define SJ_ALL(op) \
    SJ_R(op, 16, 0) SJ_R(op, 17, 1) SJ_R(op, 18, 2) SJ_R(op, 19, 3)    \
    SJ_R(op, 20, 4) SJ_R(op, 21, 5) SJ_R(op, 22, 6) SJ_R(op, 23, 7)    \
    SJ_R(op, 30, 8) SJ_R(op, 29, 9) SJ_R(op, 28, 10) SJ_R(op, 31, 11)

__asm__(
".text\n"
".set push\n"
".set noreorder\n"
".globl " SJ_SETJMP "\n"
".type " SJ_SETJMP ", @function\n"
 SJ_SETJMP ":\n"
SJ_ALL(SJ_S)
"    jr $31\n"
"    move $2, $0\n"
".size " SJ_SETJMP ", .-" SJ_SETJMP "\n"

".globl " SJ_LONGJMP "\n"
".type " SJ_LONGJMP ", @function\n"
 SJ_LONGJMP ":\n"
SJ_ALL(SJ_L)
"    sltiu $2, $5, 1\n"
"    jr $31\n"
"    addu $2, $2, $5\n"
".size " SJ_LONGJMP ", .-" SJ_LONGJMP "\n"
".set pop\n"
);

#endif
