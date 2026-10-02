/* A volatile automatic object changed between setjmp and longjmp keeps
 * its new value after the longjmp (C11 7.13.2.1p3) -- which holds only
 * if the object is in memory. The register allocator gave volatile
 * locals registers like any other, and longjmp restored the register to
 * the value it had at setjmp: this returned 1. And store forwarding
 * passed a volatile local's stored value straight to its reads, so the
 * second read here never happened. */
// expect-exit: 42
#include <setjmp.h>
static jmp_buf jb;
__attribute__((noinline)) static void jump(void) { longjmp(jb, 1); }
__attribute__((noinline)) static int run(int k)
{
    volatile int v = 1;
    volatile int n = 0;
    if (setjmp(jb) == 0) {
        v = k;
        n = n + 1;
        jump();
    }
    return v + n * 100;
}
int main(void)
{
    return run(42) == 142 ? 42 : 1;
}
