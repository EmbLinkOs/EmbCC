/* The backtrace firmware for tests/golden/debug-frame.sh: main -> outer
 * -> mid -> leaf, where mid keeps floats across its calls (so on a
 * Cortex-M4 with its FPU it saves s16 up with a vpush) and outer keeps
 * integers (so it pushes callee-saved core registers). A debugger stopped
 * in leaf must unwind through all of them to reset. Without an FPU
 * `real` is an int, so the image needs no soft-float runtime. */
extern unsigned __data_load, __data_start, __data_end;
extern unsigned __bss_start, __bss_end;
int main(void);
void reset(void);

__attribute__((section(".vectors"), used))
void *const vectors[2] = { (void *)0x20010000u, (void *)reset };

#ifdef __ARM_FP
typedef float real;
#else
typedef int real;
#endif

volatile real sink;
volatile int isink;

__attribute__((noinline)) real leaf(real x)
{
    sink = x;                                   /* LEAF-LINE */
    return x * 2;
}

__attribute__((noinline)) real mid(real a, real b)
{
    real t = leaf(a) * b;
    real u = leaf(t + a);
    return t + u + a * b;
}

__attribute__((noinline)) int outer(int n)
{
    int s = 0, k = n * 3;
    for (int i = 0; i < n; i++) {
        s += (int)mid((real)i, 2) + k;
        isink = s;
    }
    return s + k;
}

int main(void)
{
#ifdef __ARM_FP
    /* s16, callee-saved, holds pi in outer's frame: mid's vpush saves it
     * and its CFI says where, so a debugger in leaf reads it back there */
    __asm__ volatile("vmov s16, %0" : : "r"(0x40490fdbu) : "s16");
#endif
    return outer(2) & 0x7f;
}

void reset(void)
{
    unsigned *d = &__data_start, *s = &__data_load;
#ifdef __ARM_FP
    *(volatile unsigned *)0xE000ED88u |= 0xFu << 20;    /* CPACR: the FPU */
#endif
    while (d < &__data_end)
        *d++ = *s++;
    for (d = &__bss_start; d < &__bss_end;)
        *d++ = 0;
    isink = main();
    for (;;)
        ;
}
