/* <setjmp.h> — C11 §7.13.
 *
 * The one part of this library that cannot be written in C. setjmp has to
 * save the registers the calling convention says a function must preserve,
 * and longjmp has to resume at a stored address with a stored stack
 * pointer; no C expression denotes those. The implementations are machine
 * code (src/setjmp/), one file per architecture.
 *
 * The buffer is sized for the widest target rather than per-target so a
 * jmp_buf in a shared struct has the same layout everywhere: x86-64 uses
 * 8 slots, aarch64 uses 20.
 *
 * The usual warning applies and is not a defect: after longjmp, a local of
 * the setjmp-calling function that is neither `volatile` nor unmodified
 * since the setjmp has an indeterminate value (§7.13.2.1p3). EmbCC keeps
 * locals in callee-saved registers at -O2, so this is observable here, as
 * it is on every optimising compiler.
 */
#ifndef _SETJMP_H
#define _SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__APPLE__)
/* Apple's C library's setjmp, which a Darwin program calls: its size, so
 * a struct holding one is laid out as clang lays it out. */
#if defined(__aarch64__)
typedef int jmp_buf[(14 + 8 + 2) * 2];
#else
typedef int jmp_buf[(9 * 2) + 3 + 16];
#endif
#else
typedef long jmp_buf[32];
#endif

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

/* §7.13p1 requires setjmp to be a macro. It may also be a function, and
 * this is the form that gives both without a wrapper frame. */
#define setjmp(env) setjmp(env)

/* The BSD spelling, which programs use to mean "no signal mask". This
 * library has no signal mask to save, so they are the same functions. */
#define _setjmp  setjmp
#define _longjmp longjmp

#ifdef __cplusplus
}
#endif

#endif
