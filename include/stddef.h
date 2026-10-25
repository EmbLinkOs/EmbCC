/* EmbCC's stddef.h — compiler-owned (ARCHITECTURE §5: the predefined
 * macro table supplies the underlying types). */
#ifndef _STDDEF_H
#define _STDDEF_H
typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
#ifdef __cplusplus
/* wchar_t is a keyword; NULL is g++'s __null (an integer 0 that converts
 * to any pointer), and nullptr's type has its C++11 name */
typedef __WINT_TYPE__ wint_t;
typedef decltype(nullptr) nullptr_t;
#define NULL __null
#else
typedef __WCHAR_TYPE__ wchar_t;
typedef __WINT_TYPE__ wint_t;
#define NULL ((void *)0)
#endif
/* The most strictly aligned scalar type, as GCC's stddef.h lays it out,
 * in C11 as in C++: 16 bytes' alignment on x86-64, AArch64 and RISC-V,
 * 8 on Cortex-M, 1 on AVR. */
typedef struct {
    long long __max_align_ll;
    long double __max_align_ld;
} max_align_t;
/* Fold to a size_t constant via the builtin, so offsetof works in an
 * integer-constant-expression (the `&((T*)0)->m` form does not). */
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
