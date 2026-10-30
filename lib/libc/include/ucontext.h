/* <ucontext.h> -- user contexts: a thread's state saved, resumed, and
 * made to run a function on a stack of its own (coroutines' oldest
 * means). x86-64 and AArch64 (src/setjmp/ucontext-*.c); elsewhere there
 * is nothing to declare. */
#ifndef _UCONTEXT_H
#define _UCONTEXT_H

#include <sys/ucontext.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__x86_64__) || defined(__aarch64__)
int  getcontext(ucontext_t *ucp);
int  setcontext(const ucontext_t *ucp);
void makecontext(ucontext_t *ucp, void (*fn)(void), int argc, ...);
int  swapcontext(ucontext_t *__restrict oucp, const ucontext_t *__restrict ucp);
#endif

#ifdef __cplusplus
}
#endif

#endif
