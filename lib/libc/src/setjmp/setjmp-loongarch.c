/* setjmp/longjmp for LoongArch64 (LP64S: EmbCC's LoongArch is soft-float).
 *
 * The psABI calls ra ($r1), sp ($r3), fp ($r22) and s0-s8 ($r23-$r31)
 * callee-saved; $r21 is reserved and $tp the thread pointer, neither of
 * which a function changes.
 *
 *   jmp_buf: ra, sp, fp, s0-s8
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4): a0 = val +
 * (val == 0), at 32 bits as an int is kept. */
#if defined(__loongarch__)

#define SJ_STR_(x) #x
#define SJ_STR(x)  SJ_STR_(x)
#define SJ_R(op, r, k) "    " op " $" #r ", $a0, " SJ_STR(k) "*8\n"
#define SJ_ALL(op) \
    SJ_R(op, ra, 0) SJ_R(op, sp, 1) SJ_R(op, fp, 2) SJ_R(op, s0, 3)    \
    SJ_R(op, s1, 4) SJ_R(op, s2, 5) SJ_R(op, s3, 6) SJ_R(op, s4, 7)    \
    SJ_R(op, s5, 8) SJ_R(op, s6, 9) SJ_R(op, s7, 10) SJ_R(op, s8, 11)

__asm__(
".text\n"
".globl setjmp\n"
".type setjmp, @function\n"
"setjmp:\n"
SJ_ALL("st.d")
"    move $a0, $zero\n"
"    jr $ra\n"
".size setjmp, .-setjmp\n"

".globl longjmp\n"
".type longjmp, @function\n"
"longjmp:\n"
SJ_ALL("ld.d")
"    sltui $a0, $a1, 1\n"
"    add.w $a0, $a0, $a1\n"
"    jr $ra\n"
".size longjmp, .-longjmp\n"
);

#endif
