/* EmbCC's cpuid.h: x86's cpuid instruction, as GCC's <cpuid.h> gives it
 * (__cpuid, __cpuid_count, __get_cpuid_max, __get_cpuid, ...) and the
 * feature bits and vendor signatures programs test — libstdc++'s
 * random_device among them (rdrand, rdseed). */
#ifndef _CPUID_H_INCLUDED
#define _CPUID_H_INCLUDED

#if !defined __x86_64__ && !defined __i386__
#error "cpuid.h is for x86"
#endif

/* leaf 1, %ecx */
#define bit_SSE3        (1 << 0)
#define bit_PCLMUL      (1 << 1)
#define bit_LZCNT       (1 << 5)
#define bit_SSSE3       (1 << 9)
#define bit_FMA         (1 << 12)
#define bit_CMPXCHG16B  (1 << 13)
#define bit_SSE4_1      (1 << 19)
#define bit_SSE4_2      (1 << 20)
#define bit_MOVBE       (1 << 22)
#define bit_POPCNT      (1 << 23)
#define bit_AES         (1 << 25)
#define bit_XSAVE       (1 << 26)
#define bit_OSXSAVE     (1 << 27)
#define bit_AVX         (1 << 28)
#define bit_F16C        (1 << 29)
#define bit_RDRND       (1 << 30)

/* leaf 1, %edx */
#define bit_CMPXCHG8B   (1 << 8)
#define bit_CMOV        (1 << 15)
#define bit_MMX         (1 << 23)
#define bit_FXSAVE      (1 << 24)
#define bit_SSE         (1 << 25)
#define bit_SSE2        (1 << 26)

/* extended leaf 0x80000001, %ecx */
#define bit_LAHF_LM     (1 << 0)
#define bit_ABM         (1 << 5)
#define bit_SSE4a       (1 << 6)
#define bit_PRFCHW      (1 << 8)
#define bit_XOP         (1 << 11)
#define bit_LWP         (1 << 15)
#define bit_FMA4        (1 << 16)
#define bit_TBM         (1 << 21)
#define bit_MWAITX      (1 << 29)

/* extended leaf 0x80000001, %edx */
#define bit_MMXEXT      (1 << 22)
#define bit_LM          (1 << 29)
#define bit_3DNOWP      (1 << 30)
#define bit_3DNOW       (1u << 31)

/* leaf 7, sub-leaf 0, %ebx */
#define bit_FSGSBASE    (1 << 0)
#define bit_SGX         (1 << 2)
#define bit_BMI         (1 << 3)
#define bit_HLE         (1 << 4)
#define bit_AVX2        (1 << 5)
#define bit_BMI2        (1 << 8)
#define bit_RTM         (1 << 11)
#define bit_AVX512F     (1 << 16)
#define bit_AVX512DQ    (1 << 17)
#define bit_RDSEED      (1 << 18)
#define bit_ADX         (1 << 19)
#define bit_AVX512IFMA  (1 << 21)
#define bit_CLFLUSHOPT  (1 << 23)
#define bit_CLWB        (1 << 24)
#define bit_AVX512PF    (1 << 26)
#define bit_AVX512ER    (1 << 27)
#define bit_AVX512CD    (1 << 28)
#define bit_SHA         (1 << 29)
#define bit_AVX512BW    (1 << 30)
#define bit_AVX512VL    (1u << 31)

/* leaf 7, sub-leaf 0, %ecx */
#define bit_PREFETCHWT1 (1 << 0)
#define bit_AVX512VBMI  (1 << 1)
#define bit_PKU         (1 << 3)
#define bit_OSPKE       (1 << 4)
#define bit_WAITPKG     (1 << 5)
#define bit_AVX512VBMI2 (1 << 6)
#define bit_SHSTK       (1 << 7)
#define bit_GFNI        (1 << 8)
#define bit_VAES        (1 << 9)
#define bit_VPCLMULQDQ  (1 << 10)
#define bit_AVX512VNNI  (1 << 11)
#define bit_AVX512BITALG (1 << 12)
#define bit_AVX512VPOPCNTDQ (1 << 14)
#define bit_RDPID       (1 << 22)
#define bit_MOVDIRI     (1 << 27)
#define bit_MOVDIR64B   (1 << 28)

/* leaf 7, sub-leaf 0, %edx */
#define bit_AVX5124VNNIW (1 << 2)
#define bit_AVX5124FMAPS (1 << 3)
#define bit_SERIALIZE   (1 << 14)
#define bit_HYBRID      (1 << 15)
#define bit_PCONFIG     (1 << 18)
#define bit_IBT         (1 << 20)

/* leaf 0xd, sub-leaf 1, %eax */
#define bit_XSAVEOPT    (1 << 0)
#define bit_XSAVEC      (1 << 1)
#define bit_XSAVES      (1 << 3)

/* the vendor signatures leaf 0 leaves in %ebx, %ecx, %edx */
#define signature_AMD_ebx       0x68747541
#define signature_AMD_ecx       0x444d4163
#define signature_AMD_edx       0x69746e65
#define signature_INTEL_ebx     0x756e6547
#define signature_INTEL_ecx     0x6c65746e
#define signature_INTEL_edx     0x49656e69
#define signature_HYGON_ebx     0x6f677948
#define signature_HYGON_ecx     0x656e6975
#define signature_HYGON_edx     0x6e65476e
#define signature_CENTAUR_ebx   0x746e6543
#define signature_CENTAUR_ecx   0x736c7561
#define signature_CENTAUR_edx   0x48727561
#define signature_ZHAOXIN_ebx   0x68532020
#define signature_ZHAOXIN_ecx   0x20206961
#define signature_ZHAOXIN_edx   0x68676e61
#define signature_VIA_ebx       0x20414956
#define signature_VIA_ecx       0x20414956
#define signature_VIA_edx       0x20414956
#define signature_TM1_ebx       0x6e617254
#define signature_TM1_ecx       0x55504361
#define signature_TM1_edx       0x74656d73
#define signature_TM2_ebx       0x756e6547
#define signature_TM2_ecx       0x3638784d
#define signature_TM2_edx       0x54656e69

/* (the leaf in %eax, the sub-leaf in %ecx: EmbCC's asm names the
 * registers, where GCC's cpuid.h ties them to the outputs) */
#define __cpuid(level, a, b, c, d)                                      \
    __asm__ __volatile__("cpuid"                                        \
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)           \
                         : "a"(level), "c"(0))

#define __cpuid_count(level, count, a, b, c, d)                         \
    __asm__ __volatile__("cpuid"                                        \
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)           \
                         : "a"(level), "c"(count))

/* The highest leaf of the range `ext` begins (0, or 0x80000000 for the
 * extended ones); *sig, if given, gets %ebx (the vendor's first word). */
static __inline unsigned int __get_cpuid_max(unsigned int __ext,
                                             unsigned int *__sig)
{
    unsigned int __eax, __ebx, __ecx, __edx;
    __cpuid(__ext, __eax, __ebx, __ecx, __edx);
    if (__sig)
        *__sig = __ebx;
    return __eax;
}

/* Leaf `leaf`'s registers: 0 if the processor has no such leaf. */
static __inline int __get_cpuid(unsigned int __leaf, unsigned int *__eax,
                                unsigned int *__ebx, unsigned int *__ecx,
                                unsigned int *__edx)
{
    unsigned int __max = __get_cpuid_max(__leaf & 0x80000000u, 0);
    if (__max == 0 || __max < __leaf)
        return 0;
    __cpuid(__leaf, *__eax, *__ebx, *__ecx, *__edx);
    return 1;
}

static __inline int __get_cpuid_count(unsigned int __leaf,
                                      unsigned int __subleaf,
                                      unsigned int *__eax,
                                      unsigned int *__ebx,
                                      unsigned int *__ecx,
                                      unsigned int *__edx)
{
    unsigned int __max = __get_cpuid_max(__leaf & 0x80000000u, 0);
    if (__max == 0 || __max < __leaf)
        return 0;
    __cpuid_count(__leaf, __subleaf, *__eax, *__ebx, *__ecx, *__edx);
    return 1;
}

static __inline void __cpuidex(int __info[4], int __leaf, int __subleaf)
{
    __cpuid_count(__leaf, __subleaf, __info[0], __info[1], __info[2],
                  __info[3]);
}

#endif /* _CPUID_H_INCLUDED */
