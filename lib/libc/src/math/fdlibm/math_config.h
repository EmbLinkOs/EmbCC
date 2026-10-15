/* math_config.h — emlibc's minimal shim for the classic fdlibm cores. Newlib's
 * real math_config.h is a large file for its NEW (ARM optimized-routines) math;
 * the classic e_exp.c/e_pow.c/e_cosh.c/s_expm1.c we lift, and their float
 * forms ef_exp.c/ef_pow.c/ef_cosh.c/sf_expm1.c, use only the overflow and
 * underflow result helpers and the signaling-NaN test. Provide exactly those. */
#ifndef _EMLIBC_FDLIBM_MATH_CONFIG_H
#define _EMLIBC_FDLIBM_MATH_CONFIG_H

#include <math.h>
#include <errno.h>
#include <stdint.h>

/* sign==0 -> +overflow (+Inf / +0), sign!=0 -> negative. errno=ERANGE, as a
 * libm should. The multiply forces the IEEE overflow/underflow flag the way
 * newlib's helpers do; the returned value is what fdlibm's callers expect. */
static inline double __math_oflow(uint32_t sign)
{
    errno = ERANGE;
    return (sign ? -1.0 : 1.0) * HUGE_VAL;
}
static inline double __math_uflow(uint32_t sign)
{
    errno = ERANGE;
    return sign ? -0.0 : 0.0;
}

/* Signaling-NaN test newlib's e_pow.c reaches for: a NaN whose quiet bit
 * (mantissa MSB) is clear. */
static inline int issignaling_inline(double x)
{
    union { double d; uint64_t u; } v; v.d = x;
    uint64_t man = v.u & 0xfffffffffffffULL;
    return ((v.u >> 52) & 0x7ff) == 0x7ff && man != 0 && !(man & 0x8000000000000ULL);
}

/* The float forms, for the ef_/sf_ cores. */
static inline float __math_oflowf(uint32_t sign)
{
    errno = ERANGE;
    return (sign ? -1.0f : 1.0f) * HUGE_VALF;
}
static inline float __math_uflowf(uint32_t sign)
{
    errno = ERANGE;
    return sign ? -0.0f : 0.0f;
}
static inline int issignalingf_inline(float x)
{
    union { float f; uint32_t u; } v; v.f = x;
    return ((v.u >> 23) & 0xff) == 0xff && (v.u & 0x7fffff) != 0 &&
           !(v.u & 0x400000);
}

#endif /* _EMLIBC_FDLIBM_MATH_CONFIG_H */
