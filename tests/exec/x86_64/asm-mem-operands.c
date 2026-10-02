/* Memory operands in inline asm. An "m" output's register holds the
 * ADDRESS the template writes through, and nothing loaded it: the template
 * wrote through whatever the register last held -- the address only when
 * the code before happened to leave it there, as <fenv.h>'s
 * `stmxcsr %0` did. And "rm"/"g" allow a register, which GCC gives them,
 * so the template is written for the VALUE; they were read as memory and
 * handed the template the operand's address.
 *
 * EmbCC's "m" convention is that the template dereferences the register
 * itself, `movq %1, (%0)`, where GCC substitutes a memory operand -- so
 * there is no gcc reference for this program. */
// target: x86_64-elf
// expect-exit: 42
// no-gcc-reference: EmbCC's "m" operand is a register the template dereferences

__attribute__((noinline)) static unsigned long put(unsigned long k)
{
    unsigned long v = 0, junk = 0x1234;
    /* something else in the operand's likely register first */
    __asm__ volatile("movq %1, %0" : "=r"(junk) : "r"(junk));
    __asm__ volatile("movq %1, (%0)" : "=m"(v) : "r"(k));
    return v + (junk != 0x1234);
}

__attribute__((noinline)) static unsigned mxcsr(void)
{
    unsigned v = 0;
    __asm__ volatile("stmxcsr %0" : "=m"(v));
    return v;
}

__attribute__((noinline)) static int copy_rm(int x)
{
    int r;
    __asm__("mov %1, %0" : "=r"(r) : "rm"(x));
    return r;
}

__attribute__((noinline)) static int copy_g(int x)
{
    int r;
    __asm__("mov %1, %0" : "=r"(r) : "g"(x));
    return r;
}

int main(void)
{
    if (put(40) != 40) return 1;
    if ((mxcsr() & 0x1f80) != 0x1f80) return 2;      /* exceptions masked */
    if (copy_rm(41) != 41) return 3;
    if (copy_g(39) != 39) return 4;
    return 42;
}
