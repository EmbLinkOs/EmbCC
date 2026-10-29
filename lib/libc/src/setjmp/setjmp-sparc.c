/* setjmp/longjmp for SPARC V8 (LEON), with its register windows.
 *
 * A function's locals and ins live in its own window, which a deep call
 * chain spills to the function's stack and a return reloads from it (the
 * window overflow and underflow traps). So setjmp, a leaf in its
 * caller's window, records only what locates that window: its %sp, its
 * %fp and the return address in %o7.
 *
 * longjmp returns up through the windows -- `restore` after `restore`,
 * each one the underflow trap reloads when it was spilled -- until it is
 * in the window whose %fp is the recorded one: setjmp's caller's, with
 * the values its locals have now, which is all C promises (7.13.2.1p3).
 * That is glibc's way, and needs no window flush: a bare-metal trap
 * table has the overflow and underflow handlers already, and need not
 * have a `ta 3`. The walk's state is in %g1-%g3, which no restore
 * changes and EmbCC's SPARC ABI keeps as scratch.
 *
 *   jmp_buf, in words: [0] = %sp, [1] = %o7, [2] = %fp
 *
 * longjmp(env, 0) makes setjmp return 1 (C11 7.13.2.1p4). EmbCC's SPARC
 * is soft-float, and the ABI saves no FP register anyway. */
/* The symbols' names: lib/rt/builtin-setjmp.c builds this file again as
 * __builtin_setjmp's and __builtin_longjmp's own pair. */
#ifndef SJ_SETJMP
#define SJ_SETJMP  "setjmp"
#define SJ_LONGJMP "longjmp"
#endif

#if defined(__sparc__) || defined(__sparc)

__asm__(
".text\n"
".globl " SJ_SETJMP "\n"
".type " SJ_SETJMP ", #function\n"
 SJ_SETJMP ":\n"
"    st %sp, [%o0]\n"
"    st %o7, [%o0 + 4]\n"
"    st %fp, [%o0 + 8]\n"
"    retl\n"
"    mov 0, %o0\n"
".size " SJ_SETJMP ", .-" SJ_SETJMP "\n"

".globl " SJ_LONGJMP "\n"
".type " SJ_LONGJMP ", #function\n"
 SJ_LONGJMP ":\n"
"    mov %o0, %g1\n"
"    orcc %o1, %g0, %g2\n"
"    be,a 1f\n"
"    mov 1, %g2\n"                     /* only when val was 0 */
"1:  ld [%g1 + 8], %g3\n"
"2:  cmp %fp, %g3\n"
"    blu,a 2b\n"
"    restore\n"                        /* only while below the target */
"    ld [%g1], %sp\n"
"    ld [%g1 + 4], %o7\n"
"    retl\n"
"    mov %g2, %o0\n"
".size " SJ_LONGJMP ", .-" SJ_LONGJMP "\n"
);

#endif
