/* The ARM run-time ABI's helper functions (RTABI32, "Run-time ABI for the
 * Arm Architecture"), for every Cortex-M target and ARMv7-A: the names clang, GCC and
 * the vendors' toolchains call, so an object or library built by them --
 * CMSIS-DSP, a vendor HAL, anything compiled with arm-none-eabi-gcc --
 * links against this archive. EmbCC's own code calls the libgcc names
 * (__adddf3, __divdi3 ...) and none of these.
 *
 *   __aeabi_memcpy[4|8], __aeabi_memmove[4|8], __aeabi_memset[4|8],
 *   __aeabi_memclr[4|8]                      block operations
 *   __aeabi_idiv, __aeabi_uidiv, __aeabi_idivmod, __aeabi_uidivmod
 *                                            (where there is a divide
 *                                            instruction; the others' are
 *                                            in aeabidiv.c, and ARMv6-M's
 *                                            memcpy/memclr, lmul and shifts
 *                                            in armv6m.c)
 *   __aeabi_ldivmod, __aeabi_uldivmod        quotient in r0:r1, remainder
 *                                            in r2:r3
 *   __aeabi_lmul, __aeabi_llsl, __aeabi_llsr, __aeabi_lasr,
 *   __aeabi_lcmp, __aeabi_ulcmp
 *   __aeabi_{f,d}{add,sub,rsub,mul,div}, comparisons {cmpeq,cmplt,cmple,
 *   cmpge,cmpgt,cmpun}, conversions {i,ui,l,ul}2{f,d}, {f,d}2{iz,uiz,lz,
 *   ulz}, f2d, d2f                           soft-float, by forwarding to
 *                                            lib/rt/softfp.c's routines
 *
 * The floating-point helpers take and return their values in core
 * registers whatever the build's float ABI -- the RTABI says the base
 * procedure call standard -- so they are pcs("aapcs") under
 * -mfloat-abi=hard. All are weak: a program that brings its own keeps it.
 * THE RULE of lib/rt holds (README.md): the block loops have a variable
 * count, and EmbCC turns no loop into a call.
 */
#if defined(__ARM_EABI__)

#define WEAK __attribute__((weak))
#define BASE __attribute__((weak, pcs("aapcs")))

typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned long size_t;

/* ---- block operations ------------------------------------------------ */

static void copy_fwd(unsigned char *d, const unsigned char *s, size_t n)
{
    while (n--)
        *d++ = *s++;
}
static void copy_bwd(unsigned char *d, const unsigned char *s, size_t n)
{
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
}
static void fill(unsigned char *d, size_t n, int c)
{
    while (n--)
        *d++ = (unsigned char)c;
}

/* ARMv6-M's own copy and clear are in armv6m.c (ARMv8-M Baseline's too) */
#if !defined(__ARM_ARCH_6M__) && !defined(__ARM_ARCH_8M_BASE__)
WEAK void __aeabi_memcpy(void *d, const void *s, size_t n) { copy_fwd(d, s, n); }
WEAK void __aeabi_memcpy4(void *d, const void *s, size_t n)
{
    u32 *dw = d;
    const u32 *sw = s;
    for (; n >= 4; n -= 4)
        *dw++ = *sw++;
    copy_fwd((unsigned char *)dw, (const unsigned char *)sw, n);
}
WEAK void __aeabi_memclr(void *d, size_t n) { fill(d, n, 0); }
WEAK void __aeabi_memclr4(void *d, size_t n)
{
    u32 *dw = d;
    for (; n >= 4; n -= 4)
        *dw++ = 0;
    fill((unsigned char *)dw, n, 0);
}
#endif
WEAK void __aeabi_memcpy8(void *d, const void *s, size_t n)
{
    u32 *dw = d;
    const u32 *sw = s;
    for (; n >= 4; n -= 4)
        *dw++ = *sw++;
    copy_fwd((unsigned char *)dw, (const unsigned char *)sw, n);
}
WEAK void __aeabi_memmove(void *d, const void *s, size_t n)
{
    if ((unsigned char *)d <= (const unsigned char *)s)
        copy_fwd(d, s, n);
    else
        copy_bwd(d, s, n);
}
WEAK void __aeabi_memmove4(void *d, const void *s, size_t n) { __aeabi_memmove(d, s, n); }
WEAK void __aeabi_memmove8(void *d, const void *s, size_t n) { __aeabi_memmove(d, s, n); }
/* note the order: destination, length, value */
WEAK void __aeabi_memset(void *d, size_t n, int c) { fill(d, n, c); }
WEAK void __aeabi_memset4(void *d, size_t n, int c) { fill(d, n, c); }
WEAK void __aeabi_memset8(void *d, size_t n, int c) { fill(d, n, c); }
WEAK void __aeabi_memclr8(void *d, size_t n) { fill(d, n, 0); }

/* ---- integers -------------------------------------------------------- */

#if defined(__ARM_FEATURE_IDIV)
/* ARMv7-M divides in hardware: these are one sdiv/udiv each. (Without
 * the instruction `n / d` IS a call to __aeabi_idiv: aeabidiv.c.) */
WEAK s32 __aeabi_idiv(s32 n, s32 d) { return n / d; }
WEAK u32 __aeabi_uidiv(u32 n, u32 d) { return n / d; }
/* the quotient in r0 and the remainder in r1: a 64-bit return's halves */
WEAK u64 __aeabi_idivmod(s32 n, s32 d)
{
    s32 q = n / d;
    return (u64)(u32)q | (u64)(u32)(n - q * d) << 32;
}
WEAK u64 __aeabi_uidivmod(u32 n, u32 d)
{
    u32 q = n / d;
    return (u64)q | (u64)(n - q * d) << 32;
}
#endif
#if !defined(__ARM_ARCH_6M__) && !defined(__ARM_ARCH_8M_BASE__)
WEAK u64 __aeabi_lmul(u64 a, u64 b) { return a * b; }
WEAK u64 __aeabi_llsl(u64 a, int n) { return n >= 64 ? 0 : a << n; }
WEAK u64 __aeabi_llsr(u64 a, int n) { return n >= 64 ? 0 : a >> n; }
WEAK s64 __aeabi_lasr(s64 a, int n) { return a >> (n >= 64 ? 63 : n); }
#endif
WEAK int __aeabi_lcmp(s64 a, s64 b) { return a < b ? -1 : a > b; }
WEAK int __aeabi_ulcmp(u64 a, u64 b) { return a < b ? -1 : a > b; }
WEAK int __aeabi_idiv0(int r) { return r; }
WEAK long long __aeabi_ldiv0(long long r) { return r; }

/* __aeabi_[u]ldivmod return the quotient in r0:r1 and the remainder in
 * r2:r3, which no C return type expresses: a naked wrapper calls the C
 * half with a pointer to a stack slot for the remainder and loads it
 * into r2:r3. Thumb-1 forms only, so ARMv6-M takes it too. */
u64 __rt_uldivmod(u64 n, u64 d, u64 *rem)
{
    u64 q = n / d;
    *rem = n - q * d;
    return q;
}
s64 __rt_ldivmod(s64 n, s64 d, s64 *rem)
{
    s64 q = n / d;
    *rem = n - q * d;
    return q;
}
__attribute__((naked, weak)) void __aeabi_uldivmod(void)
{
    __asm__("push {r4, lr}\n\t"
            "sub sp, #16\n\t"
            "add r4, sp, #8\n\t"
            "str r4, [sp]\n\t"
            "bl __rt_uldivmod\n\t"
            "ldr r2, [sp, #8]\n\t"
            "ldr r3, [sp, #12]\n\t"
            "add sp, #16\n\t"
            "pop {r4, pc}");
}
__attribute__((naked, weak)) void __aeabi_ldivmod(void)
{
    __asm__("push {r4, lr}\n\t"
            "sub sp, #16\n\t"
            "add r4, sp, #8\n\t"
            "str r4, [sp]\n\t"
            "bl __rt_ldivmod\n\t"
            "ldr r2, [sp, #8]\n\t"
            "ldr r3, [sp, #12]\n\t"
            "add sp, #16\n\t"
            "pop {r4, pc}");
}

/* ---- soft float, in the base procedure call standard ----------------- */

/* With a DOUBLE-precision FPU (bit 3 of __ARM_FP: the Cortex-M7's
 * FPv5-D16, an ARMv7-A VFPv3/VFPv4) lib/rt/softfp.c keeps only the 64-bit
 * integer conversions, which no VFP unit has -- so these are the
 * operations themselves, one instruction each, still taking and returning
 * their operands in the core registers (BASE). Forwarding to __addsf3 and
 * the rest, as below, left every such image that pulled this member in
 * with undefined symbols. Out-of-range float-to-int conversions saturate,
 * as vcvt does and as the libgcc routines answer. */
#if defined(__ARM_FP) && (__ARM_FP & 8)
float __floatdisf(long long); float __floatundisf(unsigned long long);
double __floatdidf(long long); double __floatundidf(unsigned long long);
long long __fixsfdi(float); unsigned long long __fixunssfdi(float);
long long __fixdfdi(double); unsigned long long __fixunsdfdi(double);

BASE float __aeabi_fadd(float a, float b) { return a + b; }
BASE float __aeabi_fsub(float a, float b) { return a - b; }
BASE float __aeabi_frsub(float a, float b) { return b - a; }
BASE float __aeabi_fmul(float a, float b) { return a * b; }
BASE float __aeabi_fdiv(float a, float b) { return a / b; }
BASE double __aeabi_dadd(double a, double b) { return a + b; }
BASE double __aeabi_dsub(double a, double b) { return a - b; }
BASE double __aeabi_drsub(double a, double b) { return b - a; }
BASE double __aeabi_dmul(double a, double b) { return a * b; }
BASE double __aeabi_ddiv(double a, double b) { return a / b; }
BASE int __aeabi_fcmpeq(float a, float b) { return a == b; }
BASE int __aeabi_fcmplt(float a, float b) { return a < b; }
BASE int __aeabi_fcmple(float a, float b) { return a <= b; }
BASE int __aeabi_fcmpge(float a, float b) { return a >= b; }
BASE int __aeabi_fcmpgt(float a, float b) { return a > b; }
BASE int __aeabi_fcmpun(float a, float b) { return a != a || b != b; }
BASE int __aeabi_dcmpeq(double a, double b) { return a == b; }
BASE int __aeabi_dcmplt(double a, double b) { return a < b; }
BASE int __aeabi_dcmple(double a, double b) { return a <= b; }
BASE int __aeabi_dcmpge(double a, double b) { return a >= b; }
BASE int __aeabi_dcmpgt(double a, double b) { return a > b; }
BASE int __aeabi_dcmpun(double a, double b) { return a != a || b != b; }
BASE float __aeabi_i2f(int a) { return (float)a; }
BASE float __aeabi_ui2f(unsigned a) { return (float)a; }
BASE float __aeabi_l2f(long long a) { return __floatdisf(a); }
BASE float __aeabi_ul2f(unsigned long long a) { return __floatundisf(a); }
BASE double __aeabi_i2d(int a) { return (double)a; }
BASE double __aeabi_ui2d(unsigned a) { return (double)a; }
BASE double __aeabi_l2d(long long a) { return __floatdidf(a); }
BASE double __aeabi_ul2d(unsigned long long a) { return __floatundidf(a); }
BASE int __aeabi_f2iz(float a) { return (int)a; }
BASE unsigned __aeabi_f2uiz(float a) { return (unsigned)a; }
BASE long long __aeabi_f2lz(float a) { return __fixsfdi(a); }
BASE unsigned long long __aeabi_f2ulz(float a) { return __fixunssfdi(a); }
BASE int __aeabi_d2iz(double a) { return (int)a; }
BASE unsigned __aeabi_d2uiz(double a) { return (unsigned)a; }
BASE long long __aeabi_d2lz(double a) { return __fixdfdi(a); }
BASE unsigned long long __aeabi_d2ulz(double a) { return __fixunsdfdi(a); }
BASE double __aeabi_f2d(float a) { return (double)a; }
BASE float __aeabi_d2f(double a) { return (float)a; }
#else

float __addsf3(float, float); float __subsf3(float, float);
float __mulsf3(float, float); float __divsf3(float, float);
double __adddf3(double, double); double __subdf3(double, double);
double __muldf3(double, double); double __divdf3(double, double);
int __eqsf2(float, float); int __ltsf2(float, float); int __lesf2(float, float);
int __gesf2(float, float); int __gtsf2(float, float); int __unordsf2(float, float);
int __eqdf2(double, double); int __ltdf2(double, double); int __ledf2(double, double);
int __gedf2(double, double); int __gtdf2(double, double); int __unorddf2(double, double);
float __floatsisf(int); float __floatunsisf(unsigned); float __floatdisf(long long);
float __floatundisf(unsigned long long);
double __floatsidf(int); double __floatunsidf(unsigned); double __floatdidf(long long);
double __floatundidf(unsigned long long);
int __fixsfsi(float); unsigned __fixunssfsi(float); long long __fixsfdi(float);
unsigned long long __fixunssfdi(float);
int __fixdfsi(double); unsigned __fixunsdfsi(double); long long __fixdfdi(double);
unsigned long long __fixunsdfdi(double);
double __extendsfdf2(float); float __truncdfsf2(double);

BASE float __aeabi_fadd(float a, float b) { return __addsf3(a, b); }
BASE float __aeabi_fsub(float a, float b) { return __subsf3(a, b); }
BASE float __aeabi_frsub(float a, float b) { return __subsf3(b, a); }
BASE float __aeabi_fmul(float a, float b) { return __mulsf3(a, b); }
BASE float __aeabi_fdiv(float a, float b) { return __divsf3(a, b); }
BASE double __aeabi_dadd(double a, double b) { return __adddf3(a, b); }
BASE double __aeabi_dsub(double a, double b) { return __subdf3(a, b); }
BASE double __aeabi_drsub(double a, double b) { return __subdf3(b, a); }
BASE double __aeabi_dmul(double a, double b) { return __muldf3(a, b); }
BASE double __aeabi_ddiv(double a, double b) { return __divdf3(a, b); }

/* the comparisons answer 1 or 0; an unordered pair makes every one but
 * cmpun false, which the libgcc routines' return values give */
BASE int __aeabi_fcmpeq(float a, float b) { return __eqsf2(a, b) == 0; }
BASE int __aeabi_fcmplt(float a, float b) { return __ltsf2(a, b) < 0; }
BASE int __aeabi_fcmple(float a, float b) { return __lesf2(a, b) <= 0; }
BASE int __aeabi_fcmpge(float a, float b) { return __gesf2(a, b) >= 0; }
BASE int __aeabi_fcmpgt(float a, float b) { return __gtsf2(a, b) > 0; }
BASE int __aeabi_fcmpun(float a, float b) { return __unordsf2(a, b) != 0; }
BASE int __aeabi_dcmpeq(double a, double b) { return __eqdf2(a, b) == 0; }
BASE int __aeabi_dcmplt(double a, double b) { return __ltdf2(a, b) < 0; }
BASE int __aeabi_dcmple(double a, double b) { return __ledf2(a, b) <= 0; }
BASE int __aeabi_dcmpge(double a, double b) { return __gedf2(a, b) >= 0; }
BASE int __aeabi_dcmpgt(double a, double b) { return __gtdf2(a, b) > 0; }
BASE int __aeabi_dcmpun(double a, double b) { return __unorddf2(a, b) != 0; }

BASE float __aeabi_i2f(int a) { return __floatsisf(a); }
BASE float __aeabi_ui2f(unsigned a) { return __floatunsisf(a); }
BASE float __aeabi_l2f(long long a) { return __floatdisf(a); }
BASE float __aeabi_ul2f(unsigned long long a) { return __floatundisf(a); }
BASE double __aeabi_i2d(int a) { return __floatsidf(a); }
BASE double __aeabi_ui2d(unsigned a) { return __floatunsidf(a); }
BASE double __aeabi_l2d(long long a) { return __floatdidf(a); }
BASE double __aeabi_ul2d(unsigned long long a) { return __floatundidf(a); }
BASE int __aeabi_f2iz(float a) { return __fixsfsi(a); }
BASE unsigned __aeabi_f2uiz(float a) { return __fixunssfsi(a); }
BASE long long __aeabi_f2lz(float a) { return __fixsfdi(a); }
BASE unsigned long long __aeabi_f2ulz(float a) { return __fixunssfdi(a); }
BASE int __aeabi_d2iz(double a) { return __fixdfsi(a); }
BASE unsigned __aeabi_d2uiz(double a) { return __fixunsdfsi(a); }
BASE long long __aeabi_d2lz(double a) { return __fixdfdi(a); }
BASE unsigned long long __aeabi_d2ulz(double a) { return __fixunsdfdi(a); }
BASE double __aeabi_f2d(float a) { return __extendsfdf2(a); }
BASE float __aeabi_d2f(double a) { return __truncdfsf2(a); }

#endif /* __ARM_FP & 8 */
#endif
