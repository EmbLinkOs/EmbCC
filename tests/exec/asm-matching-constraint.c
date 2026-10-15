/* GCC's matching constraint: an input "N" placed in output N's register,
 * its value going in and output N's coming out -- with another variable
 * than the output (the common case, refused on every target), with the
 * output's own lvalue, two tied pairs, and the template's numbers after
 * the tied input (and the input itself) still naming the right operands.
 * (An input narrower than its output is left out: GCC puts whatever the
 * target's load gives in the register -- a signed char -5 reads back as
 * 251 on x86-64 -- and EmbCC converts it.) */
// expect-exit: 42
#include <stdio.h>

__attribute__((noinline)) static int add(int a, int b)
{
    int r;
#if defined(__x86_64__)
    __asm__("addl %2, %0" : "=r"(r) : "0"(a), "r"(b));
#elif defined(__aarch64__)
    __asm__("add %w0, %w1, %w2" : "=r"(r) : "0"(a), "r"(b));
#elif defined(__thumb__) || defined(__arm__)
    __asm__("add %0, %1, %2" : "=r"(r) : "0"(a), "r"(b));
#elif defined(__riscv) || defined(__mips__)
    __asm__("add %0, %1, %2" : "=r"(r) : "0"(a), "r"(b));
#else
    __asm__("" : "=r"(r) : "0"(a));
    r += b;
#endif
    return r;
}

__attribute__((noinline)) static int copy(int x)
{
    int y;
    __asm__("" : "=r"(y) : "0"(x));
    return y;
}

__attribute__((noinline)) static int self(int x)
{
    __asm__("" : "=r"(x) : "0"(x));
    return x + 1;
}

__attribute__((noinline)) static int pair(int a, int b)
{
    int p, q;
    __asm__("" : "=r"(p), "=r"(q) : "0"(a), "1"(b));
    return p * 10 + q;
}

int main(void)
{
    int ok = 0;
    ok += add(30, 12) == 42;
    ok += copy(7) == 7;
    ok += self(41) == 42;
    ok += pair(4, 2) == 42;
    printf("%d %d %d %d\n", add(30, 12), copy(7), self(41), pair(4, 2));
    return ok == 4 ? 42 : ok;
}
