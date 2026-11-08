/* RISC-V inline asm with "f" operands: a float or double in an f
 * register, which EmbCC refused ('asm constraint "=f" is not valid for
 * RISC-V'). An "=f" output, "f" inputs, a "+f" operand, a double where
 * the D extension is there, and a template that writes callee-saved fs3
 * -- which the prologue saves -- while the caller keeps a float live
 * across the calls. Elsewhere, or without F, it only returns 42. */
// expect-exit: 42
#if defined(__riscv) && defined(__riscv_flen)
static float fadd(float a, float b)
{
    float r;
    __asm__("fadd.s %0, %1, %2" : "=f"(r) : "f"(a), "f"(b));
    return r;
}
static float fneg(float a) { __asm__("fneg.s %0, %0" : "+f"(a)); return a; }
__attribute__((noinline)) static float fsq(float a)
{
    __asm__ volatile("fmv.s fs3, %0\n\tfmul.s %0, fs3, fs3" : "+f"(a) :: "fs3");
    return a;
}
#if __riscv_flen >= 64
static double fmul(double a, double b)
{
    double r;
    __asm__("fmul.d %0, %1, %2" : "=f"(r) : "f"(a), "f"(b));
    return r;
}
#endif
#endif

int main(void)
{
#if defined(__riscv) && defined(__riscv_flen)
    volatile float x = 1.5f, y = 2.25f;
    float keep = x * 3.0f;               /* live across fsq's calls */
    if (fadd(x, y) != 3.75f) return 1;
    if (fneg(x) != -1.5f) return 2;
    if (fsq(y) != 5.0625f || fsq(x) != 2.25f) return 3;
    if (keep != 4.5f) return 4;
#if __riscv_flen >= 64
    volatile double p = 3.0, q = -2.5;
    if (fmul(p, q) != -7.5) return 5;
#endif
#endif
    return 42;
}
