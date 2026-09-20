/* The floating-point environment, per target.
 *
 * Two registers and some bit twiddling, and the only reason it is not
 * trivial is that the two targets disagree about where everything is:
 *
 *   x86-64  MXCSR holds the flags in bits 0..5 and the rounding mode in
 *           bits 13..14. The x87 control word is not touched -- every
 *           float and double here goes through SSE, and long double
 *           through x87 with its own word, which this does not model.
 *
 *   aarch64 FPSR holds the flags (bits 0..4, with no denormal flag in
 *           the same place) and FPCR the rounding mode (bits 22..23).
 *           They are two registers, not one, which is why the fenv_t
 *           here has two words.
 *
 * The flag BIT ORDER differs too, so the portable FE_* values are
 * translated rather than passed through on aarch64. Passing them
 * through would work on one target and silently test the wrong bit on
 * the other.
 */
#include <fenv.h>

const fenv_t __fe_dfl_env = { 0, 0 };

#if defined(__x86_64__)

static unsigned get_mxcsr(void)
{
    unsigned v;
    __asm__ __volatile__("stmxcsr %0" : "=m"(v));
    return v;
}

static void set_mxcsr(unsigned v)
{
    __asm__ __volatile__("ldmxcsr %0" : : "m"(v));
}

/* MXCSR's flag bits ARE the FE_* values, which is why they were chosen
 * that way above: no translation on the target that matters most. */
static int read_flags(void)   { return (int)(get_mxcsr() & FE_ALL_EXCEPT); }
static void write_flags(int f)
{
    unsigned v = get_mxcsr();
    set_mxcsr((v & ~(unsigned)FE_ALL_EXCEPT) | ((unsigned)f & FE_ALL_EXCEPT));
}
static int read_round(void)   { return (int)((get_mxcsr() >> 13) & 3u); }
static void write_round(int r)
{
    unsigned v = get_mxcsr();
    set_mxcsr((v & ~(3u << 13)) | (((unsigned)r & 3u) << 13));
}
static unsigned read_control(void) { return get_mxcsr() & ~(unsigned)FE_ALL_EXCEPT; }
static void write_control(unsigned c)
{
    set_mxcsr((get_mxcsr() & (unsigned)FE_ALL_EXCEPT) | c);
}

#elif defined(__aarch64__)

static unsigned long get_fpsr(void)
{
    unsigned long v;
    __asm__ __volatile__("mrs %0, fpsr" : "=r"(v));
    return v;
}
static void set_fpsr(unsigned long v)
{
    __asm__ __volatile__("msr fpsr, %0" : : "r"(v));
}
static unsigned long get_fpcr(void)
{
    unsigned long v;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(v));
    return v;
}
static void set_fpcr(unsigned long v)
{
    __asm__ __volatile__("msr fpcr, %0" : : "r"(v));
}

/* FPSR: IOC 0, DZC 1, OFC 2, UFC 3, IXC 4, IDC 7. A different order
 * from MXCSR's, so both directions are translated. */
static int from_fpsr(unsigned long s)
{
    int f = 0;
    if (s & (1u << 0)) f |= FE_INVALID;
    if (s & (1u << 1)) f |= FE_DIVBYZERO;
    if (s & (1u << 2)) f |= FE_OVERFLOW;
    if (s & (1u << 3)) f |= FE_UNDERFLOW;
    if (s & (1u << 4)) f |= FE_INEXACT;
    if (s & (1u << 7)) f |= FE_DENORMAL;
    return f;
}
static unsigned long to_fpsr(int f)
{
    unsigned long s = 0;
    if (f & FE_INVALID)   s |= 1u << 0;
    if (f & FE_DIVBYZERO) s |= 1u << 1;
    if (f & FE_OVERFLOW)  s |= 1u << 2;
    if (f & FE_UNDERFLOW) s |= 1u << 3;
    if (f & FE_INEXACT)   s |= 1u << 4;
    if (f & FE_DENORMAL)  s |= 1u << 7;
    return s;
}

static int read_flags(void) { return from_fpsr(get_fpsr()); }
static void write_flags(int f)
{
    set_fpsr((get_fpsr() & ~to_fpsr(FE_ALL_EXCEPT)) | to_fpsr(f));
}

/* FPCR bits 22..23: 00 nearest, 01 toward +inf, 10 toward -inf, 11
 * toward zero. The portable constants are in a different order again. */
static int read_round(void)
{
    switch ((get_fpcr() >> 22) & 3u) {
    case 0: return FE_TONEAREST;
    case 1: return FE_UPWARD;
    case 2: return FE_DOWNWARD;
    default: return FE_TOWARDZERO;
    }
}
static void write_round(int r)
{
    unsigned long m;
    switch (r) {
    case FE_UPWARD:     m = 1; break;
    case FE_DOWNWARD:   m = 2; break;
    case FE_TOWARDZERO: m = 3; break;
    default:            m = 0; break;
    }
    set_fpcr((get_fpcr() & ~(3ul << 22)) | (m << 22));
}
static unsigned read_control(void) { return (unsigned)get_fpcr(); }
static void write_control(unsigned c) { set_fpcr(c); }

#else

/* A target whose floating-point environment this file does not know.
 * Every accessor reports that nothing is raised and the rounding is to
 * nearest, and every setter fails -- which is the honest answer, and
 * lets a caller that checks the return value know. */
static int read_flags(void) { return 0; }
static void write_flags(int f) { (void)f; }
static int read_round(void) { return FE_TONEAREST; }
static void write_round(int r) { (void)r; }
static unsigned read_control(void) { return 0; }
static void write_control(unsigned c) { (void)c; }

#endif

int feclearexcept(int excepts)
{
    write_flags(read_flags() & ~(excepts & FE_ALL_EXCEPT));
    return 0;
}

int fetestexcept(int excepts) { return read_flags() & excepts & FE_ALL_EXCEPT; }

/* Raising a flag SETS it rather than performing an operation that would
 * raise it. C11 allows either; setting is what a caller reporting a
 * condition from its own code wants, and it cannot trap. */
int feraiseexcept(int excepts)
{
    write_flags(read_flags() | (excepts & FE_ALL_EXCEPT));
    return 0;
}

int fegetexceptflag(fexcept_t *flagp, int excepts)
{
    if (!flagp)
        return -1;
    *flagp = (fexcept_t)(read_flags() & excepts & FE_ALL_EXCEPT);
    return 0;
}

int fesetexceptflag(const fexcept_t *flagp, int excepts)
{
    if (!flagp)
        return -1;
    int keep = read_flags() & ~(excepts & FE_ALL_EXCEPT);
    write_flags(keep | ((int)*flagp & excepts & FE_ALL_EXCEPT));
    return 0;
}

int fegetround(void) { return read_round(); }

int fesetround(int round)
{
    if (round != FE_TONEAREST && round != FE_DOWNWARD &&
        round != FE_UPWARD && round != FE_TOWARDZERO)
        return -1;
    write_round(round);
    return read_round() == round ? 0 : -1;
}

int fegetenv(fenv_t *envp)
{
    if (!envp)
        return -1;
    envp->__cw = read_control();
    envp->__sw = (unsigned)read_flags();
    return 0;
}

int fesetenv(const fenv_t *envp)
{
    if (!envp)
        return -1;
    write_control(envp->__cw);
    write_flags((int)envp->__sw);
    return 0;
}

/* Save the environment and clear the flags, so a computation can be run
 * without its exceptions being seen by whoever called us. The pair with
 * feupdateenv is how a library function hides its own intermediate
 * overflow while still reporting one that the CALLER's arguments
 * caused. */
int feholdexcept(fenv_t *envp)
{
    if (fegetenv(envp) != 0)
        return -1;
    write_flags(0);
    return 0;
}

/* Restore, then re-raise whatever happened in between -- so the flags
 * end up as the union of before and during, which is what "sticky"
 * means across a saved region. */
int feupdateenv(const fenv_t *envp)
{
    if (!envp)
        return -1;
    int raised = read_flags();
    if (fesetenv(envp) != 0)
        return -1;
    return feraiseexcept(raised);
}
