/* setjmp/longjmp for 32-bit PowerPC, EABI (EmbCC's PowerPC is soft-float).
 *
 * The EABI calls r1 (sp), r14-r31 and CR fields cr2-cr4 callee-saved,
 * with the return address in LR; r2 and r13 are the small-data bases,
 * which no function changes. stmw/lmw move r14-r31 in one instruction
 * each.
 *
 *   jmp_buf, in words: [0] = r1, [1] = LR, [2] = CR, [3..20] = r14-r31
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4). */
/* The symbols' names: lib/rt/builtin-setjmp.c builds this file again as
 * __builtin_setjmp's and __builtin_longjmp's own pair. */
#ifndef SJ_SETJMP
#define SJ_SETJMP  "setjmp"
#define SJ_LONGJMP "longjmp"
#endif

#if defined(__PPC__) || defined(__powerpc__)

__asm__(
".text\n"
".globl " SJ_SETJMP "\n"
".type " SJ_SETJMP ", @function\n"
 SJ_SETJMP ":\n"
"    mflr 0\n"
"    stw 1, 0(3)\n"
"    stw 0, 4(3)\n"
"    mfcr 0\n"
"    stw 0, 8(3)\n"
"    stmw 14, 12(3)\n"
"    li 3, 0\n"
"    blr\n"
".size " SJ_SETJMP ", .-" SJ_SETJMP "\n"

".globl " SJ_LONGJMP "\n"
".type " SJ_LONGJMP ", @function\n"
 SJ_LONGJMP ":\n"
"    lwz 1, 0(3)\n"
"    lwz 0, 4(3)\n"
"    mtlr 0\n"
"    lwz 0, 8(3)\n"
"    mtcrf 0x38, 0\n"                /* cr2, cr3 and cr4 */
"    lmw 14, 12(3)\n"
"    mr. 3, 4\n"
"    bnelr\n"
"    li 3, 1\n"
"    blr\n"
".size " SJ_LONGJMP ", .-" SJ_LONGJMP "\n"
);

#endif
