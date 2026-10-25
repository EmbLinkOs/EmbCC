/* Every callee-saved register comes back from a longjmp with what it held
 * at the setjmp -- the integer ones and the floating-point ones, on every
 * target lib/libc implements setjmp for. Compiled C cannot say which
 * registers it uses, so each target has a probe in assembly: it puts a
 * value of its own in each callee-saved register, calls setjmp, then
 * puts another value in each and calls longjmp; on the second return
 * from setjmp it checks every register and returns the first one that
 * is wrong (bit k for register k), or 0.
 *
 * A register-window machine is different by design: there longjmp
 * returns to the setjmp caller's window as it is NOW, which is what C
 * promises (7.13.2.1p3), so its probe changes nothing of its own after
 * the setjmp and takes the jump from a callee three calls down -- what
 * comes back then is the window, reloaded through the other windows.
 *
 * A target without a probe here exits 42 at once: x86-64 and AArch64,
 * whose setjmp is older than this file and whose file-scope asm EmbCC
 * takes only as data. */
// expect-exit: 42
#include <setjmp.h>

#if defined(__arm__) && (defined(__thumb2__) || !defined(__thumb__))
#define SJ_PROBE 1
/* r4-r11: bits 0-7; with an FPU s16-s31: bits 8-23 (Thumb-2 and ARM) */
#define SJ_AR_SET(k, v) "    movw r" #k ", #" #v "\n"
#if defined(__ARM_FP)
#define SJ_AR_FSET(k, v) "    movw r0, #" #v "\n    vmov s" #k ", r0\n"
#define SJ_AR_FCHK(k, v, bit) "    vmov r0, s" #k "\n    cmp r0, #" #v "\n" \
    "    beq 9f\n    orr r1, r1, #" #bit "\n9:\n"
#define SJ_AR_FALL(F, a) \
    F(16, a##9) F(17, a##10) F(18, a##11) F(19, a##12) F(20, a##13)   \
    F(21, a##14) F(22, a##15) F(23, a##16) F(24, a##17) F(25, a##18)  \
    F(26, a##19) F(27, a##20) F(28, a##21) F(29, a##22) F(30, a##23)  \
    F(31, a##24)
#define SJ_AR_FSAVE   "    vpush {s16-s31}\n"
#define SJ_AR_FREST   "    vpop {s16-s31}\n"
#define SJ_AR_FCHKALL \
    SJ_AR_FCHK(16, 9, 0x100) SJ_AR_FCHK(17, 10, 0x200)                \
    SJ_AR_FCHK(18, 11, 0x400) SJ_AR_FCHK(19, 12, 0x800)               \
    SJ_AR_FCHK(20, 13, 0x1000) SJ_AR_FCHK(21, 14, 0x2000)             \
    SJ_AR_FCHK(22, 15, 0x4000) SJ_AR_FCHK(23, 16, 0x8000)             \
    SJ_AR_FCHK(24, 17, 0x10000) SJ_AR_FCHK(25, 18, 0x20000)           \
    SJ_AR_FCHK(26, 19, 0x40000) SJ_AR_FCHK(27, 20, 0x80000)           \
    SJ_AR_FCHK(28, 21, 0x100000) SJ_AR_FCHK(29, 22, 0x200000)         \
    SJ_AR_FCHK(30, 23, 0x400000) SJ_AR_FCHK(31, 24, 0x800000)
#else
#define SJ_AR_FALL(F, a)
#define SJ_AR_FSAVE   ""
#define SJ_AR_FREST   ""
#define SJ_AR_FCHKALL ""
#endif
#define SJ_AR_ALL(S, a) \
    S(4, a##1) S(5, a##2) S(6, a##3) S(7, a##4) S(8, a##5) S(9, a##6) \
    S(10, a##7) S(11, a##8)
#define SJ_AR_CHK(k, v, bit) "    cmp r" #k ", #" #v "\n    beq 9f\n" \
    "    orr r1, r1, #" #bit "\n9:\n"
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, %function\n"
#if defined(__thumb__)
".thumb_func\n"
#endif
"sj_probe:\n"
"    push {r4-r11, lr}\n"
SJ_AR_FSAVE
"    push {r0, r1}\n"            /* env, and the stack 8-aligned */
SJ_AR_ALL(SJ_AR_SET, )
SJ_AR_FALL(SJ_AR_FSET, )
"    ldr r0, [sp]\n"
"    bl setjmp\n"
"    cmp r0, #0\n"
"    bne 1f\n"
SJ_AR_ALL(SJ_AR_SET, 10)
SJ_AR_FALL(SJ_AR_FSET, 10)
"    ldr r0, [sp]\n"
"    movs r1, #1\n"
"    bl longjmp\n"
"1:  movs r1, #0\n"
SJ_AR_CHK(4, 1, 1) SJ_AR_CHK(5, 2, 2) SJ_AR_CHK(6, 3, 4) SJ_AR_CHK(7, 4, 8)
SJ_AR_CHK(8, 5, 16) SJ_AR_CHK(9, 6, 32) SJ_AR_CHK(10, 7, 64)
SJ_AR_CHK(11, 8, 128)
SJ_AR_FCHKALL
"    mov r0, r1\n"
"    add sp, sp, #8\n"
SJ_AR_FREST
"    pop {r4-r11, pc}\n"
);

#elif defined(__thumb__)
#define SJ_PROBE 1
/* ARMv6-M and ARMv8-M Baseline: r4-r11, bits 0-7, the high ones set and
 * read through r0 */
#define SJ_T1_LO(k, v) "    movs r" #k ", #" #v "\n"
#define SJ_T1_HI(k, v) "    movs r0, #" #v "\n    mov r" #k ", r0\n"
#define SJ_T1_ALL(a) \
    SJ_T1_LO(4, a##1) SJ_T1_LO(5, a##2) SJ_T1_LO(6, a##3)             \
    SJ_T1_LO(7, a##4) SJ_T1_HI(8, a##5) SJ_T1_HI(9, a##6)             \
    SJ_T1_HI(10, a##7) SJ_T1_HI(11, a##8)
#define SJ_T1_CHKLO(k, v, bit) "    cmp r" #k ", #" #v "\n    beq 9f\n" \
    "    movs r0, #" #bit "\n    orrs r1, r0\n9:\n"
#define SJ_T1_CHKHI(k, v, bit) "    mov r0, r" #k "\n    cmp r0, #" #v "\n" \
    "    beq 9f\n    movs r0, #" #bit "\n    orrs r1, r0\n9:\n"
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, %function\n"
".thumb_func\n"
"sj_probe:\n"
"    push {r4-r7, lr}\n"
"    mov r4, r8\n mov r5, r9\n mov r6, r10\n mov r7, r11\n"
"    push {r4-r7}\n"
"    push {r0, r1}\n"
SJ_T1_ALL()
"    ldr r0, [sp]\n"
"    bl setjmp\n"
"    cmp r0, #0\n"
"    bne 1f\n"
SJ_T1_ALL(10)
"    ldr r0, [sp]\n"
"    movs r1, #1\n"
"    bl longjmp\n"
"1:  movs r1, #0\n"
SJ_T1_CHKLO(4, 1, 1) SJ_T1_CHKLO(5, 2, 2) SJ_T1_CHKLO(6, 3, 4)
SJ_T1_CHKLO(7, 4, 8) SJ_T1_CHKHI(8, 5, 16) SJ_T1_CHKHI(9, 6, 32)
SJ_T1_CHKHI(10, 7, 64) SJ_T1_CHKHI(11, 8, 128)
"    mov r0, r1\n"
"    add sp, #8\n"
"    pop {r4-r7}\n"
"    mov r8, r4\n mov r9, r5\n mov r10, r6\n mov r11, r7\n"
"    pop {r4-r7, pc}\n"
);

#elif defined(__riscv)
#define SJ_PROBE 1
/* s0-s11: bits 0-11; with F or D fs0-fs11: bits 12-23 */
#if __riscv_xlen == 64
#define SJ_RV_S "sd"
#define SJ_RV_L "ld"
#else
#define SJ_RV_S "sw"
#define SJ_RV_L "lw"
#endif
#define SJ_RV_SET(r, v) "    li " #r ", " #v "\n"
#define SJ_RV_CHK(r, v, bit) "    li t0, " #v "\n    beq " #r ", t0, 9f\n" \
    "    li t0, " #bit "\n    or a0, a0, t0\n9:\n"
#define SJ_RV_ALL(S, a) \
    S(s0, a##1) S(s1, a##2) S(s2, a##3) S(s3, a##4) S(s4, a##5)        \
    S(s5, a##6) S(s6, a##7) S(s7, a##8) S(s8, a##9) S(s9, a##10)      \
    S(s10, a##11) S(s11, a##12)
#if defined(__riscv_flen)
#if __riscv_flen == 64
#define SJ_RV_FMVTO "fmv.d.x"
#define SJ_RV_FMVFROM "fmv.x.d"
#else
#define SJ_RV_FMVTO "fmv.w.x"
#define SJ_RV_FMVFROM "fmv.x.w"
#endif
#if __riscv_flen == 64 && __riscv_xlen == 32
/* RV32 with D: no fmv between a 64-bit FP register and the integer ones,
 * so each goes through memory, its low word the pattern */
#define SJ_RV_FSET(r, v) "    li t0, " #v "\n    sw t0, 0(sp)\n" \
    "    sw zero, 4(sp)\n    fld " #r ", 0(sp)\n"
#define SJ_RV_FGET(r) "    fsd " #r ", 0(sp)\n    lw t1, 0(sp)\n"
#else
#define SJ_RV_FSET(r, v) "    li t0, " #v "\n    " SJ_RV_FMVTO " " #r ", t0\n"
#define SJ_RV_FGET(r) "    " SJ_RV_FMVFROM " t1, " #r "\n"
#endif
#define SJ_RV_FCHK(r, v, bit) SJ_RV_FGET(r) "    li t0, " #v "\n" \
    "    beq t1, t0, 9f\n    li t0, " #bit "\n    or a0, a0, t0\n9:\n"
#define SJ_RV_FALL(F, a) \
    F(fs0, a##13) F(fs1, a##14) F(fs2, a##15) F(fs3, a##16)            \
    F(fs4, a##17) F(fs5, a##18) F(fs6, a##19) F(fs7, a##20)            \
    F(fs8, a##21) F(fs9, a##22) F(fs10, a##23) F(fs11, a##24)
#if __riscv_flen == 64
#define SJ_RV_FS "fsd"
#define SJ_RV_FL "fld"
#else
#define SJ_RV_FS "fsw"
#define SJ_RV_FL "flw"
#endif
#define SJ_RV_FSV(r, k) "    " SJ_RV_FS " " #r ", " #k "*8+112(sp)\n"
#define SJ_RV_FLD(r, k) "    " SJ_RV_FL " " #r ", " #k "*8+112(sp)\n"
#define SJ_RV_FSAVE(M) \
    M(fs0, 0) M(fs1, 1) M(fs2, 2) M(fs3, 3) M(fs4, 4) M(fs5, 5)        \
    M(fs6, 6) M(fs7, 7) M(fs8, 8) M(fs9, 9) M(fs10, 10) M(fs11, 11)
#define SJ_RV_FCHKALL \
    SJ_RV_FCHK(fs0, 13, 0x1000) SJ_RV_FCHK(fs1, 14, 0x2000)            \
    SJ_RV_FCHK(fs2, 15, 0x4000) SJ_RV_FCHK(fs3, 16, 0x8000)            \
    SJ_RV_FCHK(fs4, 17, 0x10000) SJ_RV_FCHK(fs5, 18, 0x20000)          \
    SJ_RV_FCHK(fs6, 19, 0x40000) SJ_RV_FCHK(fs7, 20, 0x80000)          \
    SJ_RV_FCHK(fs8, 21, 0x100000) SJ_RV_FCHK(fs9, 22, 0x200000)        \
    SJ_RV_FCHK(fs10, 23, 0x400000) SJ_RV_FCHK(fs11, 24, 0x800000)
#else
#define SJ_RV_FALL(F, a)
#define SJ_RV_FSAVE(M)
#define SJ_RV_FCHKALL ""
#endif
/* the frame, 16-aligned: [0] scratch, [8] env, [16] ra, [24..112) s0-s11
 * at 8 apart, [112..208) fs0-fs11 */
#define SJ_RV_GSV(r, k) "    " SJ_RV_S " " #r ", " #k "*8+16(sp)\n"
#define SJ_RV_GLD(r, k) "    " SJ_RV_L " " #r ", " #k "*8+16(sp)\n"
#define SJ_RV_GSAVE(M) \
    M(ra, 0) M(s0, 1) M(s1, 2) M(s2, 3) M(s3, 4) M(s4, 5) M(s5, 6)     \
    M(s6, 7) M(s7, 8) M(s8, 9) M(s9, 10) M(s10, 11) M(s11, 12)
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, @function\n"
"sj_probe:\n"
"    addi sp, sp, -208\n"
SJ_RV_GSAVE(SJ_RV_GSV)
SJ_RV_FSAVE(SJ_RV_FSV)
"    " SJ_RV_S " a0, 8(sp)\n"
SJ_RV_ALL(SJ_RV_SET, )
SJ_RV_FALL(SJ_RV_FSET, )
"    " SJ_RV_L " a0, 8(sp)\n"
"    call setjmp\n"
"    bnez a0, 1f\n"
SJ_RV_ALL(SJ_RV_SET, 10)
SJ_RV_FALL(SJ_RV_FSET, 10)
"    " SJ_RV_L " a0, 8(sp)\n"
"    li a1, 1\n"
"    call longjmp\n"
"1:  li a0, 0\n"
SJ_RV_CHK(s0, 1, 0x1) SJ_RV_CHK(s1, 2, 0x2) SJ_RV_CHK(s2, 3, 0x4)
SJ_RV_CHK(s3, 4, 0x8) SJ_RV_CHK(s4, 5, 0x10) SJ_RV_CHK(s5, 6, 0x20)
SJ_RV_CHK(s6, 7, 0x40) SJ_RV_CHK(s7, 8, 0x80) SJ_RV_CHK(s8, 9, 0x100)
SJ_RV_CHK(s9, 10, 0x200) SJ_RV_CHK(s10, 11, 0x400)
SJ_RV_CHK(s11, 12, 0x800)
SJ_RV_FCHKALL
SJ_RV_GSAVE(SJ_RV_GLD)
SJ_RV_FSAVE(SJ_RV_FLD)
"    addi sp, sp, 208\n"
"    ret\n"
);

#elif defined(__mips__)
#define SJ_PROBE 1
/* s0-s7 and fp: bits 0-8 (gp is the small-data base, left alone) */
#if _MIPS_SIM == _ABI64
#define SJ_MS "sd"
#define SJ_ML "ld"
#define SJ_MADD "daddiu"
#else
#define SJ_MS "sw"
#define SJ_ML "lw"
#define SJ_MADD "addiu"
#endif
#define SJ_M_SET(r, v) "    addiu $" #r ", $0, " #v "\n"
#define SJ_M_ALL(a) \
    SJ_M_SET(16, a##1) SJ_M_SET(17, a##2) SJ_M_SET(18, a##3)          \
    SJ_M_SET(19, a##4) SJ_M_SET(20, a##5) SJ_M_SET(21, a##6)          \
    SJ_M_SET(22, a##7) SJ_M_SET(23, a##8) SJ_M_SET(30, a##9)
#define SJ_M_CHK(r, v, bit) "    addiu $8, $0, " #v "\n" \
    "    beq $" #r ", $8, 9f\n    nop\n    ori $2, $2, " #bit "\n9:\n"
/* the frame: [0..16) the argument area o32 gives a callee, [16] env,
 * [24] ra, [32..104) s0-s7 and fp */
#define SJ_M_SV(r, o) "    " SJ_MS " $" #r ", " #o "($29)\n"
#define SJ_M_LD(r, o) "    " SJ_ML " $" #r ", " #o "($29)\n"
#define SJ_M_FRAME(M) \
    M(31, 24) M(16, 32) M(17, 40) M(18, 48) M(19, 56) M(20, 64)       \
    M(21, 72) M(22, 80) M(23, 88) M(30, 96)
__asm__(
".text\n"
".set push\n"
".set noreorder\n"
".globl sj_probe\n"
".type sj_probe, @function\n"
"sj_probe:\n"
"    " SJ_MADD " $29, $29, -112\n"
SJ_M_FRAME(SJ_M_SV)
"    " SJ_MS " $4, 16($29)\n"
SJ_M_ALL()
"    jal setjmp\n"
"    nop\n"
"    bne $2, $0, 1f\n"
"    nop\n"
SJ_M_ALL(10)
"    " SJ_ML " $4, 16($29)\n"
"    jal longjmp\n"
"    addiu $5, $0, 1\n"
"1:  move $2, $0\n"
SJ_M_CHK(16, 1, 1) SJ_M_CHK(17, 2, 2) SJ_M_CHK(18, 3, 4) SJ_M_CHK(19, 4, 8)
SJ_M_CHK(20, 5, 16) SJ_M_CHK(21, 6, 32) SJ_M_CHK(22, 7, 64)
SJ_M_CHK(23, 8, 128) SJ_M_CHK(30, 9, 256)
SJ_M_FRAME(SJ_M_LD)
"    jr $31\n"
"    " SJ_MADD " $29, $29, 112\n"
".set pop\n"
);

#elif defined(__loongarch__)
#define SJ_PROBE 1
/* s0-s8 and fp: bits 0-9 */
#define SJ_LA_SET(r, v) "    addi.w $" #r ", $zero, " #v "\n"
#define SJ_LA_ALL(a) \
    SJ_LA_SET(s0, a##1) SJ_LA_SET(s1, a##2) SJ_LA_SET(s2, a##3)       \
    SJ_LA_SET(s3, a##4) SJ_LA_SET(s4, a##5) SJ_LA_SET(s5, a##6)       \
    SJ_LA_SET(s6, a##7) SJ_LA_SET(s7, a##8) SJ_LA_SET(s8, a##9)       \
    SJ_LA_SET(fp, a##10)
#define SJ_LA_CHK(r, v, bit) "    addi.w $t0, $zero, " #v "\n" \
    "    beq $" #r ", $t0, 9f\n    ori $a0, $a0, " #bit "\n9:\n"
#define SJ_LA_FRAME(M) \
    M(ra, 0) M(s0, 16) M(s1, 24) M(s2, 32) M(s3, 40) M(s4, 48)        \
    M(s5, 56) M(s6, 64) M(s7, 72) M(s8, 80) M(fp, 88)
#define SJ_LA_SV(r, o) "    st.d $" #r ", $sp, " #o "\n"
#define SJ_LA_LD(r, o) "    ld.d $" #r ", $sp, " #o "\n"
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, @function\n"
"sj_probe:\n"
"    addi.d $sp, $sp, -112\n"
SJ_LA_FRAME(SJ_LA_SV)
"    st.d $a0, $sp, 8\n"
SJ_LA_ALL()
"    bl setjmp\n"
"    bnez $a0, 1f\n"
SJ_LA_ALL(10)
"    ld.d $a0, $sp, 8\n"
"    addi.w $a1, $zero, 1\n"
"    bl longjmp\n"
"1:  move $a0, $zero\n"
SJ_LA_CHK(s0, 1, 1) SJ_LA_CHK(s1, 2, 2) SJ_LA_CHK(s2, 3, 4)
SJ_LA_CHK(s3, 4, 8) SJ_LA_CHK(s4, 5, 16) SJ_LA_CHK(s5, 6, 32)
SJ_LA_CHK(s6, 7, 64) SJ_LA_CHK(s7, 8, 128) SJ_LA_CHK(s8, 9, 256)
SJ_LA_CHK(fp, 10, 512)
SJ_LA_FRAME(SJ_LA_LD)
"    addi.d $sp, $sp, 112\n"
"    jr $ra\n"
);

#elif defined(__PPC__) || defined(__powerpc__)
#define SJ_PROBE 1
/* r14-r31: bits 0-17; cr2-cr4: bit 18 */
#define SJ_P_SET(r, v) "    li " #r ", " #v "\n"
#define SJ_P_ALL(a) \
    SJ_P_SET(14, a##1) SJ_P_SET(15, a##2) SJ_P_SET(16, a##3)          \
    SJ_P_SET(17, a##4) SJ_P_SET(18, a##5) SJ_P_SET(19, a##6)          \
    SJ_P_SET(20, a##7) SJ_P_SET(21, a##8) SJ_P_SET(22, a##9)          \
    SJ_P_SET(23, a##10) SJ_P_SET(24, a##11) SJ_P_SET(25, a##12)       \
    SJ_P_SET(26, a##13) SJ_P_SET(27, a##14) SJ_P_SET(28, a##15)       \
    SJ_P_SET(29, a##16) SJ_P_SET(30, a##17) SJ_P_SET(31, a##18)
#define SJ_P_CHK(r, v, op, bit) "    cmpwi " #r ", " #v "\n    beq 9f\n" \
    "    " op " 3, 3, " #bit "\n9:\n"
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, @function\n"
"sj_probe:\n"
"    stwu 1, -112(1)\n"
"    mflr 0\n"
"    stw 0, 116(1)\n"
"    stmw 14, 24(1)\n"
"    mfcr 0\n"
"    stw 0, 96(1)\n"
"    stw 3, 8(1)\n"
SJ_P_ALL()
"    lis 0, 0x005a\n"              /* cr2 = 5, cr3 = 0xa, cr4 = 3 */
"    ori 0, 0, 0x3000\n"
"    mtcrf 0x38, 0\n"
"    bl setjmp\n"
"    cmpwi 3, 0\n"
"    bne 1f\n"
SJ_P_ALL(10)
"    lis 0, 0x00a5\n"
"    ori 0, 0, 0xc000\n"
"    mtcrf 0x38, 0\n"
"    lwz 3, 8(1)\n"
"    li 4, 1\n"
"    bl longjmp\n"
"1:  mfcr 4\n"                   /* before a cmpwi writes cr0 */
"    li 3, 0\n"
SJ_P_CHK(14, 1, "ori", 1) SJ_P_CHK(15, 2, "ori", 2)
SJ_P_CHK(16, 3, "ori", 4) SJ_P_CHK(17, 4, "ori", 8)
SJ_P_CHK(18, 5, "ori", 16) SJ_P_CHK(19, 6, "ori", 32)
SJ_P_CHK(20, 7, "ori", 64) SJ_P_CHK(21, 8, "ori", 128)
SJ_P_CHK(22, 9, "ori", 256) SJ_P_CHK(23, 10, "ori", 512)
SJ_P_CHK(24, 11, "ori", 1024) SJ_P_CHK(25, 12, "ori", 2048)
SJ_P_CHK(26, 13, "ori", 4096) SJ_P_CHK(27, 14, "ori", 8192)
SJ_P_CHK(28, 15, "ori", 16384) SJ_P_CHK(29, 16, "ori", 32768)
SJ_P_CHK(30, 17, "oris", 1) SJ_P_CHK(31, 18, "oris", 2)
"    lis 5, 0x00ff\n"
"    ori 5, 5, 0xf000\n"
"    and 4, 4, 5\n"
"    lis 5, 0x005a\n"
"    ori 5, 5, 0x3000\n"
"    cmpw 4, 5\n"
"    beq 9f\n"
"    oris 3, 3, 4\n"
"9:  lmw 14, 24(1)\n"
"    lwz 0, 96(1)\n"
"    mtcrf 0x38, 0\n"
"    lwz 0, 116(1)\n"
"    mtlr 0\n"
"    addi 1, 1, 112\n"
"    blr\n"
);

#elif defined(__mcoldfire__) || defined(__m68k__)
#define SJ_PROBE 1
/* d2-d7: bits 0-5, a2-a6: bits 6-10 */
#define SJ_CF_D(r, v) "    moveq #" #v ", %" #r "\n"
#define SJ_CF_A(r, v) "    move.l #" #v ", %" #r "\n"
#define SJ_CF_ALL(a) \
    SJ_CF_D(d2, a##1) SJ_CF_D(d3, a##2) SJ_CF_D(d4, a##3)             \
    SJ_CF_D(d5, a##4) SJ_CF_D(d6, a##5) SJ_CF_D(d7, a##6)             \
    SJ_CF_A(a2, a##7) SJ_CF_A(a3, a##8) SJ_CF_A(a4, a##9)             \
    SJ_CF_A(a5, a##10) SJ_CF_A(a6, a##11)
#define SJ_CF_CHK(r, v, bit) "    move.l %" #r ", %d1\n" \
    "    cmp.l #" #v ", %d1\n    beq 9f\n    ori.l #" #bit ", %d0\n9:\n"
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, @function\n"
"sj_probe:\n"
"    move.l 4(%sp), %a0\n"
"    lea -48(%sp), %sp\n"
"    movem.l %d2-%d7/%a2-%a6, (%sp)\n"
"    move.l %a0, 44(%sp)\n"
SJ_CF_ALL()
"    move.l %a0, -(%sp)\n"
"    jsr setjmp\n"
"    addq.l #4, %sp\n"
"    tst.l %d0\n"
"    bne 1f\n"
SJ_CF_ALL(10)
"    moveq #1, %d0\n"
"    move.l %d0, -(%sp)\n"
"    move.l 48(%sp), %a0\n"
"    move.l %a0, -(%sp)\n"
"    jsr longjmp\n"
"1:  moveq #0, %d0\n"
SJ_CF_CHK(d2, 1, 1) SJ_CF_CHK(d3, 2, 2) SJ_CF_CHK(d4, 3, 4)
SJ_CF_CHK(d5, 4, 8) SJ_CF_CHK(d6, 5, 16) SJ_CF_CHK(d7, 6, 32)
SJ_CF_CHK(a2, 7, 64) SJ_CF_CHK(a3, 8, 128) SJ_CF_CHK(a4, 9, 256)
SJ_CF_CHK(a5, 10, 512) SJ_CF_CHK(a6, 11, 1024)
"    movem.l (%sp), %d2-%d7/%a2-%a6\n"
"    lea 48(%sp), %sp\n"
"    rts\n"
);

#elif defined(__RX__)
#define SJ_PROBE 1
/* r6-r13: bits 0-7 (the ABI's C names begin with an underscore) */
#define SJ_RX_SET(r, v) "    mov.l #" #v ", r" #r "\n"
#define SJ_RX_ALL(a) \
    SJ_RX_SET(6, a##1) SJ_RX_SET(7, a##2) SJ_RX_SET(8, a##3)          \
    SJ_RX_SET(9, a##4) SJ_RX_SET(10, a##5) SJ_RX_SET(11, a##6)        \
    SJ_RX_SET(12, a##7) SJ_RX_SET(13, a##8)
#define SJ_RX_CHK(r, v, bit) "    cmp #" #v ", r" #r "\n    beq 9f\n" \
    "    or #" #bit ", r1\n9:\n"
__asm__(
".text\n"
".globl _sj_probe\n"
".type _sj_probe, @function\n"
"_sj_probe:\n"
"    pushm r6-r13\n"
"    push.l r1\n"
SJ_RX_ALL()
"    bsr _setjmp\n"
"    cmp #0, r1\n"
"    bne 1f\n"
SJ_RX_ALL(10)
"    mov.l [r0], r1\n"
"    mov.l #1, r2\n"
"    bsr _longjmp\n"
"1:  mov.l #0, r1\n"
SJ_RX_CHK(6, 1, 1) SJ_RX_CHK(7, 2, 2) SJ_RX_CHK(8, 3, 4) SJ_RX_CHK(9, 4, 8)
SJ_RX_CHK(10, 5, 16) SJ_RX_CHK(11, 6, 32) SJ_RX_CHK(12, 7, 64)
SJ_RX_CHK(13, 8, 128)
"    add #4, r0\n"
"    popm r6-r13\n"
"    rts\n"
);

#elif defined(__sparc__) || defined(__sparc)
#define SJ_PROBE 1
/* the probe's window, %l0-%l7 and %i1-%i5: bits 0-12. Set before the
 * setjmp and not changed after it; the jump comes from sj_deep, ten
 * calls down -- deeper than QEMU's LEON3 has windows, so this one is
 * spilled, and the walk back reloads it. sj_deep coming back (bit 13)
 * means longjmp landed in one of its frames, which then returned. */
#define SJ_SP_CHK(r, v, bit) "    cmp %" #r ", " #v "\n    be 9f\n    nop\n" \
    "    or %g1, " #bit ", %g1\n9:\n"
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, #function\n"
"sj_probe:\n"
"    save %sp, -96, %sp\n"
"    mov 1, %l0\n mov 2, %l1\n mov 3, %l2\n mov 4, %l3\n"
"    mov 5, %l4\n mov 6, %l5\n mov 7, %l6\n mov 8, %l7\n"
"    mov 9, %i1\n mov 10, %i2\n mov 11, %i3\n mov 12, %i4\n"
"    mov 13, %i5\n"
"    call setjmp\n"
"    mov %i0, %o0\n"
"    cmp %o0, 0\n"
"    bne 1f\n"
"    nop\n"
"    mov 10, %o1\n"
"    call sj_deep\n"
"    mov %i0, %o0\n"
"    mov 1, %g1\n"                    /* sj_deep returned: bit 13 */
"    ba 8f\n"
"    sll %g1, 13, %g1\n"
"1:  mov 0, %g1\n"
SJ_SP_CHK(l0, 1, 1) SJ_SP_CHK(l1, 2, 2) SJ_SP_CHK(l2, 3, 4)
SJ_SP_CHK(l3, 4, 8) SJ_SP_CHK(l4, 5, 16) SJ_SP_CHK(l5, 6, 32)
SJ_SP_CHK(l6, 7, 64) SJ_SP_CHK(l7, 8, 128) SJ_SP_CHK(i1, 9, 256)
SJ_SP_CHK(i2, 10, 512) SJ_SP_CHK(i3, 11, 1024) SJ_SP_CHK(i4, 12, 2048)
"    cmp %i5, 13\n    be 9f\n    nop\n    mov 1, %g2\n    sll %g2, 12, %g2\n"
"    or %g1, %g2, %g1\n9:\n"
"8:  ret\n"
"    restore %g1, 0, %o0\n"
);
void sj_deep(jmp_buf env, int n);
void sj_deep(jmp_buf env, int n)
{
    volatile int clobber[8];
    for (int k = 0; k < 8; k++)
        clobber[k] = n * 31 + k;
    if (n == 0)
        longjmp(env, 1);
    sj_deep(env, n - 1);
    clobber[0] = 0;
}

#elif defined(__AVR__)
#define SJ_PROBE 1
/* r2-r17 and Y (r28:r29): an int has 16 bits here, too few for a mask of
 * eighteen, so this probe returns the first wrong register's number + 1
 * (1 for r2 .. 16 for r17, 17 and 18 for r28 and r29). */
#define SJ_PROBE_INDEX 1
#define SJ_AVR_SET(r, v) "    ldi r30, " #v "\n    mov r" #r ", r30\n"
#define SJ_AVR_ALL(v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12,  \
                   v13, v14, v15, v16, v17, v18)                       \
    SJ_AVR_SET(2, v1) SJ_AVR_SET(3, v2) SJ_AVR_SET(4, v3)             \
    SJ_AVR_SET(5, v4) SJ_AVR_SET(6, v5) SJ_AVR_SET(7, v6)             \
    SJ_AVR_SET(8, v7) SJ_AVR_SET(9, v8) SJ_AVR_SET(10, v9)            \
    SJ_AVR_SET(11, v10) SJ_AVR_SET(12, v11) SJ_AVR_SET(13, v12)       \
    SJ_AVR_SET(14, v13) SJ_AVR_SET(15, v14) SJ_AVR_SET(16, v15)       \
    SJ_AVR_SET(17, v16) SJ_AVR_SET(28, v17) SJ_AVR_SET(29, v18)
#define SJ_AVR_CHK(r, v) "    mov r30, r" #r "\n    cpi r30, " #v "\n" \
    "    breq 9f\n    ldi r24, " #v "\n    rjmp 8f\n9:\n"
#define SJ_AVR_SAVE(op) \
    op(2) op(3) op(4) op(5) op(6) op(7) op(8) op(9) op(10) op(11)     \
    op(12) op(13) op(14) op(15) op(16) op(17) op(28) op(29)
#define SJ_AVR_PUSH(r) "    push r" #r "\n"
#define SJ_AVR_POP(r)  "    pop r" #r "\n"
#define SJ_AVR_RESTORE \
    SJ_AVR_POP(29) SJ_AVR_POP(28) SJ_AVR_POP(17) SJ_AVR_POP(16)      \
    SJ_AVR_POP(15) SJ_AVR_POP(14) SJ_AVR_POP(13) SJ_AVR_POP(12)      \
    SJ_AVR_POP(11) SJ_AVR_POP(10) SJ_AVR_POP(9) SJ_AVR_POP(8)        \
    SJ_AVR_POP(7) SJ_AVR_POP(6) SJ_AVR_POP(5) SJ_AVR_POP(4)          \
    SJ_AVR_POP(3) SJ_AVR_POP(2)
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, @function\n"
"sj_probe:\n"
SJ_AVR_SAVE(SJ_AVR_PUSH)
"    push r24\n"                       /* env, kept on the stack */
"    push r25\n"
SJ_AVR_ALL(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18)
"    pop r25\n    pop r24\n    push r24\n    push r25\n"
"    call setjmp\n"
"    cp r24, r1\n"
"    cpc r25, r1\n"
"    brne 1f\n"
SJ_AVR_ALL(101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112,
           113, 114, 115, 116, 117, 118)
"    pop r25\n    pop r24\n    push r24\n    push r25\n"
"    ldi r22, 1\n"
"    ldi r23, 0\n"
"    call longjmp\n"
"1:\n"
SJ_AVR_CHK(2, 1) SJ_AVR_CHK(3, 2) SJ_AVR_CHK(4, 3) SJ_AVR_CHK(5, 4)
SJ_AVR_CHK(6, 5) SJ_AVR_CHK(7, 6) SJ_AVR_CHK(8, 7) SJ_AVR_CHK(9, 8)
SJ_AVR_CHK(10, 9) SJ_AVR_CHK(11, 10) SJ_AVR_CHK(12, 11)
SJ_AVR_CHK(13, 12) SJ_AVR_CHK(14, 13) SJ_AVR_CHK(15, 14)
SJ_AVR_CHK(16, 15) SJ_AVR_CHK(17, 16) SJ_AVR_CHK(28, 17)
SJ_AVR_CHK(29, 18)
"    ldi r24, 0\n"
"8:  clr r25\n"
"    pop r0\n    pop r0\n"
SJ_AVR_RESTORE
"    ret\n"
);

#elif defined(__TRICORE__)
#define SJ_PROBE 1
/* The upper context's D8-D15 and A12-A15: bits 0-11. Set, setjmp, set
 * again, and the jump from sj_deep ten calls down, whose contexts
 * longjmp gives back; sj_deep coming back is bit 12. */
#define SJ_TC_DSET(r, v) "    mov d" #r ", " #v "\n"
#define SJ_TC_ASET(r, v) "    mov d0, " #v "\n    mov.a a" #r ", d0\n"
#define SJ_TC_ALL(a) \
    SJ_TC_DSET(8, a##1) SJ_TC_DSET(9, a##2) SJ_TC_DSET(10, a##3)      \
    SJ_TC_DSET(11, a##4) SJ_TC_DSET(12, a##5) SJ_TC_DSET(13, a##6)    \
    SJ_TC_DSET(14, a##7) SJ_TC_DSET(15, a##8) SJ_TC_ASET(12, a##9)    \
    SJ_TC_ASET(13, a##10) SJ_TC_ASET(14, a##11) SJ_TC_ASET(15, a##12)
#define SJ_TC_DCHK(r, v, bit) "    mov d0, " #v "\n    jeq d" #r ", d0, 9f\n" \
    "    mov.u d1, " #bit "\n    or d2, d2, d1\n9:\n"
#define SJ_TC_ACHK(r, v, bit) "    mov.d d0, a" #r "\n    mov d1, " #v "\n" \
    "    jeq d0, d1, 9f\n    mov.u d1, " #bit "\n    or d2, d2, d1\n9:\n"
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, @function\n"
"sj_probe:\n"
"    lea a10, [a10]-8\n"
"    st.a [a10]0, a4\n"               /* env */
SJ_TC_ALL()
"    call setjmp\n"
"    jnz d2, 1f\n"
SJ_TC_ALL(10)
"    ld.a a4, [a10]0\n"
"    mov d4, 10\n"
"    call sj_deep\n"
"    mov.u d2, 4096\n"
"    j 8f\n"
"1:  mov d2, 0\n"
SJ_TC_DCHK(8, 1, 1) SJ_TC_DCHK(9, 2, 2) SJ_TC_DCHK(10, 3, 4)
SJ_TC_DCHK(11, 4, 8) SJ_TC_DCHK(12, 5, 16) SJ_TC_DCHK(13, 6, 32)
SJ_TC_DCHK(14, 7, 64) SJ_TC_DCHK(15, 8, 128) SJ_TC_ACHK(12, 9, 256)
SJ_TC_ACHK(13, 10, 512) SJ_TC_ACHK(14, 11, 1024) SJ_TC_ACHK(15, 12, 2048)
"8:  lea a10, [a10]8\n"
"    ret\n"
);
void sj_deep(jmp_buf env, int n);
void sj_deep(jmp_buf env, int n)
{
    volatile int clobber[8];
    for (int k = 0; k < 8; k++)
        clobber[k] = n * 31 + k;
    if (n == 0)
        longjmp(env, 1);
    sj_deep(env, n - 1);
    clobber[0] = 0;
}

#elif defined(__XTENSA__)
#define SJ_PROBE 1
/* The probe's a2-a7, which a call8 leaves to it: bits 0-5. Set, setjmp,
 * set again, and the jump from sj_deep ten calls down -- deeper than the
 * register file -- which longjmp's spill and the underflow handler have
 * to bring back. sj_deep coming back is bit 6. */
#define SJ_XT_CHK(r, v, bit) "    movi a8, " #v "\n    beq a" #r ", a8, 9f\n" \
    "    movi a8, " #bit "\n    or a9, a9, a8\n9:\n"
__asm__(
".text\n"
".globl sj_probe\n"
".type sj_probe, @function\n"
".align 4\n"
"sj_probe:\n"
"    entry a1, 32\n"
"    s32i a2, a1, 0\n"               /* env */
"    movi a2, 1\n movi a3, 2\n movi a4, 3\n movi a5, 4\n movi a6, 5\n"
"    movi a7, 6\n"
"    l32i a10, a1, 0\n"
"    call8 setjmp\n"
"    bnez a10, 1f\n"
"    movi a2, 101\n movi a3, 102\n movi a4, 103\n movi a5, 104\n"
"    movi a6, 105\n movi a7, 106\n"
"    l32i a10, a1, 0\n"
"    movi a11, 10\n"
"    call8 sj_deep\n"
"    movi a9, 64\n"
"    j 8f\n"
"1:  movi a9, 0\n"
SJ_XT_CHK(2, 1, 1) SJ_XT_CHK(3, 2, 2) SJ_XT_CHK(4, 3, 4)
SJ_XT_CHK(5, 4, 8) SJ_XT_CHK(6, 5, 16) SJ_XT_CHK(7, 6, 32)
"8:  mov a2, a9\n"
"    retw\n"
);
void sj_deep(jmp_buf env, int n);
void sj_deep(jmp_buf env, int n)
{
    volatile int clobber[8];
    for (int k = 0; k < 8; k++)
        clobber[k] = n * 31 + k;
    if (n == 0)
        longjmp(env, 1);
    sj_deep(env, n - 1);
    clobber[0] = 0;
}
#endif

#if defined(SJ_PROBE)
int sj_probe(jmp_buf env);
static jmp_buf probe_env;
#endif

int main(void)
{
#if defined(SJ_PROBE_INDEX)
    int r = sj_probe(probe_env);
    if (r != 0)
        return r;                       /* that register did not come back */
#elif defined(SJ_PROBE)
    unsigned long m = (unsigned long)(unsigned)sj_probe(probe_env);
    for (int k = 0; k < 32; k++)
        if (m >> k & 1)
            return 1 + k;               /* register k did not come back */
#endif
    return 42;
}
