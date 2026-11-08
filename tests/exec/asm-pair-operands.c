/* 64-bit inline asm operands on ARM: a register pair, with GCC's %Q (the
 * low word's register), %R (the high word's) and %H (the pair's second
 * register), and the "l" (r0-r7) class every 16-bit Thumb instruction
 * wants. EmbCC refused the modifiers ("not supported for ARMv7-M"), the
 * "l" letter, and any output wider than a register. On ARMv7: `umull
 * %Q0, %R0, %1, %2`, a 64-bit add with carry over "+r", `ldrd %0, %H0`;
 * on every ARM: the high half of an input, a swap of the halves into an
 * early-clobber output, and "+l" with a carry chain. Elsewhere the
 * program only returns 42. */
// expect-exit: 42
typedef unsigned long long u64;

#if defined(__arm__)
static unsigned hi(u64 x) { unsigned r; __asm__("mov %0, %R1" : "=r"(r) : "r"(x)); return r; }
static u64 swap(u64 a)
{
    u64 r;
    __asm__("mov %Q0, %R1\n\tmov %R0, %Q1" : "=&r"(r) : "r"(a));
    return r;
}
static u64 inc(u64 a)
{
    __asm__("adds %Q0, #1\n\tmovs r3, #0\n\tadcs %R0, r3" : "+l"(a) :: "r3", "cc");
    return a;
}
#if !defined(__ARM_ARCH_6M__) && !defined(__ARM_ARCH_8M_BASE__)
static u64 mul(unsigned a, unsigned b)
{
    u64 r;
    __asm__("umull %Q0, %R0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
static u64 add64(u64 a, u64 b)
{
    __asm__("adds %Q0, %Q0, %Q1\n\tadc %R0, %R0, %R1" : "+r"(a) : "r"(b));
    return a;
}
static u64 ldd(const u64 *p)
{
    u64 r;
    __asm__ volatile("ldrd %0, %H0, [%1]" : "=r"(r) : "r"(p));
    return r;
}
#endif
#endif

int main(void)
{
#if defined(__arm__)
    volatile u64 x = 0x123456789ull, y = 0xfffffffffull;
    if (hi(x) != 1) return 1;
    if (swap(x) != 0x2345678900000001ull) return 2;
    if (inc(0xffffffffull) != 0x100000000ull || inc(x) != 0x12345678aull) return 3;
#if !defined(__ARM_ARCH_6M__) && !defined(__ARM_ARCH_8M_BASE__)
    volatile unsigned a = 0xffffffffu, b = 3;
    u64 m = 0x1234567890abcdefull;
    if (mul(a, b) != (u64)0xffffffffu * 3) return 4;
    if (add64(x, y) != 0x123456789ull + 0xfffffffffull) return 5;
    if (ldd(&m) != m) return 6;
#endif
#endif
    return 42;
}
