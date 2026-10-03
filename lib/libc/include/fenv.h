/* <fenv.h> — C11 §7.6.
 *
 * The floating-point environment: the sticky exception flags a
 * computation leaves behind, and the rounding direction it is done in.
 * Both live in a hardware register -- MXCSR on x86-64, FPCR/FPSR on
 * aarch64 -- and this header is the portable way to reach them.
 *
 * The flags are STICKY, and that is what makes them useful: a long
 * computation can be run without checking anything, and one test at the
 * end says whether an overflow or an inexact result happened anywhere
 * in it. Testing after every operation would cost more than the
 * operations.
 *
 * Changing the rounding direction is the part with a sharp edge, and
 * C11 §7.6.1p2 states it: a program that does so without
 * `#pragma STDC FENV_ACCESS ON` has undefined behaviour, because the
 * optimizer is otherwise entitled to constant-fold an expression at
 * compile time using the default rounding. EmbCC folds in the default
 * direction, so the pragma is accepted and the caution is real.
 */
#ifndef _FENV_H
#define _FENV_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__APPLE__)
/* A Darwin program links Apple's C library, whose fegetenv and the rest
 * read and write Apple's types with Apple's flag values -- a 16-byte
 * fenv_t, where this library's is 8, and rounding modes that are FPCR or
 * x87 control-word bits rather than 0..3. Taken from the SDK's <fenv.h>. */
#if defined(__aarch64__)
typedef struct {
    unsigned long long __fpsr;
    unsigned long long __fpcr;
} fenv_t;
typedef unsigned short fexcept_t;
#define FE_INEXACT     0x0010
#define FE_UNDERFLOW   0x0008
#define FE_OVERFLOW    0x0004
#define FE_DIVBYZERO   0x0002
#define FE_INVALID     0x0001
#define FE_FLUSHTOZERO 0x0080
#define FE_ALL_EXCEPT  0x009f
#define FE_TONEAREST   0x00000000
#define FE_UPWARD      0x00400000
#define FE_DOWNWARD    0x00800000
#define FE_TOWARDZERO  0x00C00000
#else
typedef struct {
    unsigned short __control;
    unsigned short __status;
    unsigned int   __mxcsr;
    char           __reserved[8];
} fenv_t;
typedef unsigned short fexcept_t;
#define FE_INEXACT         0x0020
#define FE_UNDERFLOW       0x0010
#define FE_OVERFLOW        0x0008
#define FE_DIVBYZERO       0x0004
#define FE_INVALID         0x0001
#define FE_DENORMALOPERAND 0x0002
#define FE_ALL_EXCEPT      0x003f
#define FE_TONEAREST       0x0000
#define FE_DOWNWARD        0x0400
#define FE_UPWARD          0x0800
#define FE_TOWARDZERO      0x0c00
#endif
extern const fenv_t _FE_DFL_ENV;
#define FE_DFL_ENV (&_FE_DFL_ENV)
#else
/* The exception flags, as the hardware numbers them. On x86-64 these
 * are the MXCSR bits directly, which is what makes the accessors a
 * couple of instructions rather than a translation table. */
#define FE_INVALID   0x01
#define FE_DENORMAL  0x02
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW  0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT   0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DENORMAL | FE_DIVBYZERO | \
                       FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

/* The four IEEE rounding directions. FE_TONEAREST is ties-to-even and
 * is the default everywhere -- see docs/manual/libraries.md on why ties
 * matter. */
#define FE_TONEAREST  0
#define FE_DOWNWARD   1
#define FE_UPWARD     2
#define FE_TOWARDZERO 3

typedef unsigned long fexcept_t;

typedef struct {
    unsigned int __cw;        /* the control word: rounding and masks */
    unsigned int __sw;        /* the status word: the sticky flags */
} fenv_t;

extern const fenv_t __fe_dfl_env;
#define FE_DFL_ENV (&__fe_dfl_env)

#endif

int feclearexcept(int excepts);
int fetestexcept(int excepts);
int feraiseexcept(int excepts);
int fegetexceptflag(fexcept_t *flagp, int excepts);
int fesetexceptflag(const fexcept_t *flagp, int excepts);

int fegetround(void);
int fesetround(int round);

int fegetenv(fenv_t *envp);
int fesetenv(const fenv_t *envp);
int feholdexcept(fenv_t *envp);
int feupdateenv(const fenv_t *envp);

#ifdef __cplusplus
}
#endif

#endif
