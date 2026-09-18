/* "+" (read-write) inline-asm operands on x86-64: the register must hold the
 * lvalue's CURRENT value when the template starts. The x86 path never loaded
 * it, so `asm("addq $1,%0" : "+r"(x))` computed on whatever the register held
 * (the output's own address, as it happened). Also: "+" on a fixed register,
 * and read-write beside a plain output and an input.
 */
// target: x86_64-elf   (AT&T x86-64 asm; not an aarch64 program)
// expect-exit: 42
typedef unsigned long u64;
int main(void)
{
    u64 x = 41;
    __asm__("addq $1, %0" : "+r"(x));
    if (x != 42) return 1;

    int y = 10;
    __asm__("addl $5, %0" : "+a"(y));               /* fixed register, eax */
    if (y != 15) return 2;

    u64 acc = 100, out = 0, in = 7;
    __asm__("addq %2, %0\n\tmovq %0, %1" : "+r"(acc), "=r"(out) : "r"(in));
    if (acc != 107 || out != 107) return 3;

    unsigned int u = 0xFFFFFFFFu;                   /* a 4-byte "+r" wraps */
    __asm__("addl $1, %0" : "+r"(u));
    if (u != 0) return 4;
    return 42;
}
