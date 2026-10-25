/* setjmp/longjmp for RISC-V, RV32 and RV64.
 *
 * The psABI calls ra, sp and s0-s11 callee-saved, and with the F or D
 * extension fs0-fs11 (f8, f9, f18-f27) at their own width. One body: the
 * word size picks sw/lw or sd/ld, and FLEN fsw/flw or fsd/fld -- so the
 * soft-float library and the ilp32f/ilp32d/lp64f/lp64d ones are each this
 * file. The registers are XLEN apart from offset 0, and the FP ones
 * FLEN/8 apart after them, which keeps an fsd 8-aligned on RV32 (56).
 *
 *   jmp_buf: ra, sp, s0-s11, then fs0-fs11
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4): a0 = val +
 * (val == 0). */
#if defined(__riscv)

#if __riscv_xlen == 64
#define SJ_S  "sd"
#define SJ_L  "ld"
#define SJ_X  8
#else
#define SJ_S  "sw"
#define SJ_L  "lw"
#define SJ_X  4
#endif
#define SJ_STR_(x) #x
#define SJ_STR(x)  SJ_STR_(x)
/* register r at slot k of the integer area, and FP register r at slot k
 * of the FP area */
#define SJ_GPR(op, r, k) \
    "    " op " " r ", " SJ_STR(k) "*" SJ_STR(SJ_X) "(a0)\n"
#if defined(__riscv_flen)
#if __riscv_flen == 64
#define SJ_FS "fsd"
#define SJ_FL "fld"
#define SJ_F  8
#else
#define SJ_FS "fsw"
#define SJ_FL "flw"
#define SJ_F  4
#endif
#define SJ_FPR(op, r, k) \
    "    " op " " r ", 14*" SJ_STR(SJ_X) "+" SJ_STR(k) "*" SJ_STR(SJ_F)  \
    "(a0)\n"
#define SJ_FP_ALL(op) \
    SJ_FPR(op, "fs0", 0) SJ_FPR(op, "fs1", 1) SJ_FPR(op, "fs2", 2)     \
    SJ_FPR(op, "fs3", 3) SJ_FPR(op, "fs4", 4) SJ_FPR(op, "fs5", 5)     \
    SJ_FPR(op, "fs6", 6) SJ_FPR(op, "fs7", 7) SJ_FPR(op, "fs8", 8)     \
    SJ_FPR(op, "fs9", 9) SJ_FPR(op, "fs10", 10) SJ_FPR(op, "fs11", 11)
#else
#define SJ_FP_ALL(op) ""
#define SJ_FS ""
#define SJ_FL ""
#endif
#define SJ_GP_ALL(op) \
    SJ_GPR(op, "ra", 0) SJ_GPR(op, "sp", 1) SJ_GPR(op, "s0", 2)        \
    SJ_GPR(op, "s1", 3) SJ_GPR(op, "s2", 4) SJ_GPR(op, "s3", 5)        \
    SJ_GPR(op, "s4", 6) SJ_GPR(op, "s5", 7) SJ_GPR(op, "s6", 8)        \
    SJ_GPR(op, "s7", 9) SJ_GPR(op, "s8", 10) SJ_GPR(op, "s9", 11)      \
    SJ_GPR(op, "s10", 12) SJ_GPR(op, "s11", 13)

__asm__(
".text\n"
".globl setjmp\n"
".type setjmp, @function\n"
"setjmp:\n"
SJ_GP_ALL(SJ_S)
SJ_FP_ALL(SJ_FS)
"    li a0, 0\n"
"    ret\n"
".size setjmp, .-setjmp\n"

".globl longjmp\n"
".type longjmp, @function\n"
"longjmp:\n"
SJ_GP_ALL(SJ_L)
SJ_FP_ALL(SJ_FL)
"    seqz a0, a1\n"
"    add a0, a0, a1\n"
"    ret\n"
".size longjmp, .-longjmp\n"
);

#endif
