/* EmbCC's <arm_cmse.h>: ACLE's CMSE support for ARMv8-M (Mainline and
 * Baseline), as clang's and GCC's headers define it.
 *
 * __ARM_FEATURE_CMSE says which half is here: bit 0, every ARMv8-M part, the
 * TT instruction (cmse_TT, cmse_TTT and what is built on them); bit 1, -mcmse,
 * the Secure side -- TTA/TTAT, the Secure fields of cmse_address_info_t, and
 * the Non-secure function pointers.
 *
 * The test-target instructions are inline assembly here (EmbCC's Thumb
 * assembler has tt, ttt, tta and ttat), where clang uses builtins.
 * cmse_nonsecure_caller() is not provided: it needs the return address's
 * bit 0 as the function was entered, which EmbCC's Thumb backends do not
 * expose (__builtin_return_address is refused there); a use of it fails to
 * compile by name.
 */
#ifndef _ARM_CMSE_H
#define _ARM_CMSE_H

#if !defined(__ARM_FEATURE_CMSE) || !(__ARM_FEATURE_CMSE & 1)
#error "<arm_cmse.h> is for ARMv8-M (thumbv8m.main-none-eabi or thumbv8m.base-none-eabi)"
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What TT returns, little-endian (EmbCC's only Arm byte order). */
typedef union {
    struct cmse_address_info {
#if __ARM_FEATURE_CMSE & 2
        unsigned mpu_region : 8;
        unsigned sau_region : 8;
        unsigned mpu_region_valid : 1;
        unsigned sau_region_valid : 1;
        unsigned read_ok : 1;
        unsigned readwrite_ok : 1;
        unsigned nonsecure_read_ok : 1;
        unsigned nonsecure_readwrite_ok : 1;
        unsigned secure : 1;
        unsigned idau_region_valid : 1;
        unsigned idau_region : 8;
#else
        unsigned mpu_region : 8;
        unsigned : 8;
        unsigned mpu_region_valid : 1;
        unsigned : 1;
        unsigned read_ok : 1;
        unsigned readwrite_ok : 1;
        unsigned : 12;
#endif
    } flags;
    unsigned value;
} cmse_address_info_t;

static __inline__ cmse_address_info_t cmse_TT(void *__p)
{
    cmse_address_info_t __r;
    __asm__ volatile("tt %0, %1" : "=r"(__r.value) : "r"(__p));
    return __r;
}
static __inline__ cmse_address_info_t cmse_TTT(void *__p)
{
    cmse_address_info_t __r;
    __asm__ volatile("ttt %0, %1" : "=r"(__r.value) : "r"(__p));
    return __r;
}
#define cmse_TT_fptr(p)  cmse_TT((void *)(uintptr_t)(p))
#define cmse_TTT_fptr(p) cmse_TTT((void *)(uintptr_t)(p))

#if __ARM_FEATURE_CMSE & 2
static __inline__ cmse_address_info_t cmse_TTA(void *__p)
{
    cmse_address_info_t __r;
    __asm__ volatile("tta %0, %1" : "=r"(__r.value) : "r"(__p));
    return __r;
}
static __inline__ cmse_address_info_t cmse_TTAT(void *__p)
{
    cmse_address_info_t __r;
    __asm__ volatile("ttat %0, %1" : "=r"(__r.value) : "r"(__p));
    return __r;
}
#define cmse_TTA_fptr(p)  cmse_TTA((void *)(uintptr_t)(p))
#define cmse_TTAT_fptr(p) cmse_TTAT((void *)(uintptr_t)(p))
#endif

/* The flags of cmse_check_address_range, ACLE's values. */
#define CMSE_MPU_READWRITE 1
#define CMSE_AU_NONSECURE  2
#define CMSE_MPU_UNPRIV    4
#define CMSE_MPU_READ      8
#define CMSE_MPU_NONSECURE 16
#define CMSE_NONSECURE     (CMSE_AU_NONSECURE | CMSE_MPU_NONSECURE)

/* Is [p, p + size) one object with the permissions `flags` asks for? p, or
 * NULL when not -- ACLE's algorithm: the range must not wrap, both ends must
 * answer TT the same way (no MPU, SAU or IDAU region boundary inside it; one
 * TT when the range stays within a 32-byte granule), and the answer must
 * grant what is asked. */
static __inline__ void *cmse_check_address_range(void *__pb, size_t __s,
                                                 int __flags)
{
    uintptr_t __begin = (uintptr_t)__pb;
    uintptr_t __end = __begin + __s - 1;
    int __single;
    void *__pe;
    cmse_address_info_t __permb, __perme;
    if (__end < __begin)
        return NULL;
    __single = (__begin ^ __end) < 0x20u;
    __pe = (void *)__end;
    switch (__flags & (CMSE_MPU_UNPRIV | CMSE_MPU_NONSECURE)) {
    case 0:
        __permb = cmse_TT(__pb);
        __perme = __single ? __permb : cmse_TT(__pe);
        break;
    case CMSE_MPU_UNPRIV:
        __permb = cmse_TTT(__pb);
        __perme = __single ? __permb : cmse_TTT(__pe);
        break;
#if __ARM_FEATURE_CMSE & 2
    case CMSE_MPU_NONSECURE:
        __permb = cmse_TTA(__pb);
        __perme = __single ? __permb : cmse_TTA(__pe);
        break;
    case CMSE_MPU_UNPRIV | CMSE_MPU_NONSECURE:
        __permb = cmse_TTAT(__pb);
        __perme = __single ? __permb : cmse_TTAT(__pe);
        break;
#endif
    default:
        return NULL;
    }
    if (__permb.value != __perme.value)
        return NULL;
#if !(__ARM_FEATURE_CMSE & 2)
    if (__flags & CMSE_AU_NONSECURE)
        return NULL;
#endif
    switch (__flags & ~(CMSE_MPU_UNPRIV | CMSE_MPU_NONSECURE)) {
#if __ARM_FEATURE_CMSE & 2
    case CMSE_MPU_READ | CMSE_MPU_READWRITE | CMSE_AU_NONSECURE:
    case CMSE_MPU_READWRITE | CMSE_AU_NONSECURE:
        return __permb.flags.nonsecure_readwrite_ok ? __pb : NULL;
    case CMSE_MPU_READ | CMSE_AU_NONSECURE:
        return __permb.flags.nonsecure_read_ok ? __pb : NULL;
    case CMSE_AU_NONSECURE:
        return __permb.flags.secure ? NULL : __pb;
#endif
    case CMSE_MPU_READ | CMSE_MPU_READWRITE:
    case CMSE_MPU_READWRITE:
        return __permb.flags.readwrite_ok ? __pb : NULL;
    case CMSE_MPU_READ:
        return __permb.flags.read_ok ? __pb : NULL;
    default:
        return NULL;
    }
}

#define cmse_check_pointed_object(p, f) \
    ((__typeof__(p))cmse_check_address_range((p), sizeof(*(p)), (f)))

#if __ARM_FEATURE_CMSE & 2
/* A Non-secure function pointer has bit 0 clear: BLXNS to it leaves the
 * Secure state. */
#define cmse_nsfptr_create(p) ((__typeof__(p))((uintptr_t)(p) & ~(uintptr_t)1))
#define cmse_is_nsfptr(p)     (!((uintptr_t)(p) & 1))
/* not provided: see the top of this file */
#define cmse_nonsecure_caller() \
    __cmse_nonsecure_caller_is_not_supported_by_EmbCC()
#endif

#ifdef __cplusplus
}
#endif

#endif /* _ARM_CMSE_H */
