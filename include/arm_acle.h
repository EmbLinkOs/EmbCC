/* EmbCC's <arm_acle.h>: the DSP and SIMD32 part of ACLE (the Arm C
 * Language Extensions) -- the saturating, packing, parallel and dual-16-bit
 * multiply intrinsics -- under the names and signatures clang's header gives
 * them, for the Cortex-M parts that have the instructions and for ARMv7-A
 * in ARM state (armv7a-none-eabi).
 *
 * Each intrinsic is one instruction of EmbCC's ARM assembler (Thumb or ARM
 * state, the same templates) in an inline asm template, where clang calls a
 * __builtin_arm_*; the guards are ACLE's: __ARM_FEATURE_SAT (every ARMv7-M,
 * ARMv8-M Mainline and ARMv7-A part) for __ssat and __usat,
 * __ARM_FEATURE_DSP and __ARM_FEATURE_SIMD32 (ARMv7E-M, -mcpu=cortex-m4/m7;
 * ARMv8-M Mainline with the extension, -mcpu=cortex-m33; ARMv7-A) for the
 * rest. On a part without them the names are simply not declared, as with
 * clang.
 *
 * Not here yet: ACLE's other sections (barriers, hints, __clz, __rev and the
 * rest, the coprocessor and system-register intrinsics).
 * __ssat, __usat, __ssat16 and __usat16 take their bound as a constant, as
 * the instructions do, so they are macros.
 */
#ifndef __ARM_ACLE_H
#define __ARM_ACLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__arm__)
#define __ACLE_INL static __inline__ __attribute__((__always_inline__))
#define __ACLE_2(r, name, insn, ta, tb) \
    __ACLE_INL r name(ta __a, tb __b) { \
        r __r; \
        __asm__(insn " %0, %1, %2" : "=r"(__r) : "r"(__a), "r"(__b)); \
        return __r; }
#define __ACLE_3(r, name, insn, ta, tb, tc) \
    __ACLE_INL r name(ta __a, tb __b, tc __c) { \
        r __r; \
        __asm__(insn " %0, %1, %2, %3" : "=r"(__r) \
                : "r"(__a), "r"(__b), "r"(__c)); \
        return __r; }
/* the long accumulates: RdLo and RdHi are the accumulator's halves */
#define __ACLE_L(name, insn) \
    __ACLE_INL int64_t name(int16x2_t __a, int16x2_t __b, int64_t __c) { \
        uint32_t __lo = (uint32_t)__c, __hi = (uint32_t)((uint64_t)__c >> 32); \
        __asm__(insn " %0, %1, %2, %3" : "+r"(__lo), "+r"(__hi) \
                : "r"(__a), "r"(__b)); \
        return (int64_t)((uint64_t)__hi << 32 | __lo); }

/* 8.4.1 Width-specified saturation */
#if defined(__ARM_FEATURE_SAT) && __ARM_FEATURE_SAT
#define __ssat(x, y) __extension__ ({ int32_t __r_; \
    __asm__("ssat %0, %1, %2" : "=r"(__r_) : "I"(y), "r"((int32_t)(x))); \
    __r_; })
#define __usat(x, y) __extension__ ({ uint32_t __r_; \
    __asm__("usat %0, %1, %2" : "=r"(__r_) : "I"(y), "r"((int32_t)(x))); \
    __r_; })
#endif

#if defined(__ARM_FEATURE_DSP) && __ARM_FEATURE_DSP
/* 8.4.2 Saturating addition and subtraction (they set the Q flag) */
__ACLE_2(int32_t, __qadd, "qadd", int32_t, int32_t)
__ACLE_2(int32_t, __qsub, "qsub", int32_t, int32_t)
__ACLE_INL int32_t __qdbl(int32_t __t) { return __qadd(__t, __t); }

/* 8.4.3 Accumulating multiplications */
__ACLE_3(int32_t, __smlabb, "smlabb", int32_t, int32_t, int32_t)
__ACLE_3(int32_t, __smlabt, "smlabt", int32_t, int32_t, int32_t)
__ACLE_3(int32_t, __smlatb, "smlatb", int32_t, int32_t, int32_t)
__ACLE_3(int32_t, __smlatt, "smlatt", int32_t, int32_t, int32_t)
__ACLE_3(int32_t, __smlawb, "smlawb", int32_t, int32_t, int32_t)
__ACLE_3(int32_t, __smlawt, "smlawt", int32_t, int32_t, int32_t)
#endif

#if defined(__ARM_FEATURE_SIMD32) && __ARM_FEATURE_SIMD32
typedef int32_t int8x4_t;
typedef int32_t int16x2_t;
typedef uint32_t uint8x4_t;
typedef uint32_t uint16x2_t;

/* 8.5.4 Parallel 16-bit saturation */
#define __ssat16(x, y) __extension__ ({ int16x2_t __r_; \
    __asm__("ssat16 %0, %1, %2" : "=r"(__r_) : "I"(y), "r"((int16x2_t)(x))); \
    __r_; })
#define __usat16(x, y) __extension__ ({ uint16x2_t __r_; \
    __asm__("usat16 %0, %1, %2" : "=r"(__r_) : "I"(y), "r"((int16x2_t)(x))); \
    __r_; })

/* 8.5.5 Packing and unpacking */
__ACLE_2(int16x2_t, __sxtab16, "sxtab16", int16x2_t, int8x4_t)
__ACLE_2(int16x2_t, __uxtab16, "uxtab16", int16x2_t, int8x4_t)
__ACLE_INL int16x2_t __sxtb16(int8x4_t __a)
{ int16x2_t __r; __asm__("sxtb16 %0, %1" : "=r"(__r) : "r"(__a)); return __r; }
__ACLE_INL int16x2_t __uxtb16(int8x4_t __a)
{ int16x2_t __r; __asm__("uxtb16 %0, %1" : "=r"(__r) : "r"(__a)); return __r; }

/* 8.5.6 Parallel selection, by the GE bits the last parallel add or
 * subtract set */
__ACLE_INL uint8x4_t __sel(uint8x4_t __a, uint8x4_t __b)
{ uint8x4_t __r; __asm__ volatile("sel %0, %1, %2" : "=r"(__r) : "r"(__a), "r"(__b)); return __r; }

/* 8.5.7 Parallel 8-bit addition and subtraction. The ones that set the
 * GE bits are volatile: a later __sel reads them, so they may be neither
 * dropped nor moved past it. */
#define __ACLE_GE(r, name, insn, t) \
    __ACLE_INL r name(t __a, t __b) { \
        r __r; \
        __asm__ volatile(insn " %0, %1, %2" : "=r"(__r) : "r"(__a), "r"(__b)); \
        return __r; }
__ACLE_2(int8x4_t, __qadd8, "qadd8", int8x4_t, int8x4_t)
__ACLE_2(int8x4_t, __qsub8, "qsub8", int8x4_t, int8x4_t)
__ACLE_GE(int8x4_t, __sadd8, "sadd8", int8x4_t)
__ACLE_2(int8x4_t, __shadd8, "shadd8", int8x4_t, int8x4_t)
__ACLE_2(int8x4_t, __shsub8, "shsub8", int8x4_t, int8x4_t)
__ACLE_GE(int8x4_t, __ssub8, "ssub8", int8x4_t)
__ACLE_GE(uint8x4_t, __uadd8, "uadd8", uint8x4_t)
__ACLE_2(uint8x4_t, __uhadd8, "uhadd8", uint8x4_t, uint8x4_t)
__ACLE_2(uint8x4_t, __uhsub8, "uhsub8", uint8x4_t, uint8x4_t)
__ACLE_2(uint8x4_t, __uqadd8, "uqadd8", uint8x4_t, uint8x4_t)
__ACLE_2(uint8x4_t, __uqsub8, "uqsub8", uint8x4_t, uint8x4_t)
__ACLE_GE(uint8x4_t, __usub8, "usub8", uint8x4_t)

/* 8.5.8 Sum of 8-bit absolute differences */
__ACLE_2(uint32_t, __usad8, "usad8", uint8x4_t, uint8x4_t)
__ACLE_3(uint32_t, __usada8, "usada8", uint8x4_t, uint8x4_t, uint32_t)

/* 8.5.9 Parallel 16-bit addition and subtraction */
__ACLE_2(int16x2_t, __qadd16, "qadd16", int16x2_t, int16x2_t)
__ACLE_2(int16x2_t, __qasx, "qasx", int16x2_t, int16x2_t)
__ACLE_2(int16x2_t, __qsax, "qsax", int16x2_t, int16x2_t)
__ACLE_2(int16x2_t, __qsub16, "qsub16", int16x2_t, int16x2_t)
__ACLE_GE(int16x2_t, __sadd16, "sadd16", int16x2_t)
__ACLE_GE(int16x2_t, __sasx, "sasx", int16x2_t)
__ACLE_2(int16x2_t, __shadd16, "shadd16", int16x2_t, int16x2_t)
__ACLE_2(int16x2_t, __shasx, "shasx", int16x2_t, int16x2_t)
__ACLE_2(int16x2_t, __shsax, "shsax", int16x2_t, int16x2_t)
__ACLE_2(int16x2_t, __shsub16, "shsub16", int16x2_t, int16x2_t)
__ACLE_GE(int16x2_t, __ssax, "ssax", int16x2_t)
__ACLE_GE(int16x2_t, __ssub16, "ssub16", int16x2_t)
__ACLE_GE(uint16x2_t, __uadd16, "uadd16", uint16x2_t)
__ACLE_GE(uint16x2_t, __uasx, "uasx", uint16x2_t)
__ACLE_2(uint16x2_t, __uhadd16, "uhadd16", uint16x2_t, uint16x2_t)
__ACLE_2(uint16x2_t, __uhasx, "uhasx", uint16x2_t, uint16x2_t)
__ACLE_2(uint16x2_t, __uhsax, "uhsax", uint16x2_t, uint16x2_t)
__ACLE_2(uint16x2_t, __uhsub16, "uhsub16", uint16x2_t, uint16x2_t)
__ACLE_2(uint16x2_t, __uqadd16, "uqadd16", uint16x2_t, uint16x2_t)
__ACLE_2(uint16x2_t, __uqasx, "uqasx", uint16x2_t, uint16x2_t)
__ACLE_2(uint16x2_t, __uqsax, "uqsax", uint16x2_t, uint16x2_t)
__ACLE_2(uint16x2_t, __uqsub16, "uqsub16", uint16x2_t, uint16x2_t)
__ACLE_GE(uint16x2_t, __usax, "usax", uint16x2_t)
__ACLE_GE(uint16x2_t, __usub16, "usub16", uint16x2_t)

/* 8.5.10 Parallel 16-bit multiplication */
__ACLE_3(int32_t, __smlad, "smlad", int16x2_t, int16x2_t, int32_t)
__ACLE_3(int32_t, __smladx, "smladx", int16x2_t, int16x2_t, int32_t)
__ACLE_L(__smlald, "smlald")
__ACLE_L(__smlaldx, "smlaldx")
__ACLE_3(int32_t, __smlsd, "smlsd", int16x2_t, int16x2_t, int32_t)
__ACLE_3(int32_t, __smlsdx, "smlsdx", int16x2_t, int16x2_t, int32_t)
__ACLE_L(__smlsld, "smlsld")
__ACLE_L(__smlsldx, "smlsldx")
__ACLE_2(int32_t, __smuad, "smuad", int16x2_t, int16x2_t)
__ACLE_2(int32_t, __smuadx, "smuadx", int16x2_t, int16x2_t)
__ACLE_2(int32_t, __smusd, "smusd", int16x2_t, int16x2_t)
__ACLE_2(int32_t, __smusdx, "smusdx", int16x2_t, int16x2_t)
#undef __ACLE_GE
#endif

#undef __ACLE_2
#undef __ACLE_3
#undef __ACLE_L
#endif /* __arm__ */

#ifdef __cplusplus
}
#endif

#endif
