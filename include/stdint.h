/* EmbCC's stdint.h -- freestanding, and derived ENTIRELY from the macros the
 * compiler predefines for the target, which is how GCC's and clang's own
 * headers do it.
 *
 * This file used to hard-wire the widths to x86-64 SysV -- "EmbCC targets
 * exactly one machine, so there is nothing to vary" -- and it stayed that way
 * for five targets after that stopped being true:
 *
 *   int64_t  was `long`: FOUR bytes on ARMv7-M, ARMv8-M, RV32 and AVR,
 *            where long is 32 bits. uint64_t and intmax_t likewise.
 *   int32_t  was `int`: TWO bytes on AVR, where int is 16 bits.
 *   intptr_t was `long`: four bytes on AVR, whose pointers are two.
 *
 * Every program on those targets that used a fixed-width type to get a fixed
 * width got a different one, silently -- a 64-bit timestamp computed in 32
 * bits, a uint32_t tick counter wrapping at 65535. None of the tests saw it,
 * because every one of them spelled `long long` out. tests/golden/stdint.sh
 * now checks every type's size and every limit against the reference compiler
 * on every target.
 *
 * The predefined macros come from src/arch/<target>/predef.c, which is
 * generated from the reference compiler for that exact target
 * (tools/gen-predef.sh), so there is exactly one place a target's type widths
 * are decided. */
#ifndef _STDINT_H
#define _STDINT_H

/* ---- exact-width -------------------------------------------------------- */
typedef __INT8_TYPE__    int8_t;
typedef __INT16_TYPE__   int16_t;
typedef __INT32_TYPE__   int32_t;
typedef __INT64_TYPE__   int64_t;
typedef __UINT8_TYPE__   uint8_t;
typedef __UINT16_TYPE__  uint16_t;
typedef __UINT32_TYPE__  uint32_t;
typedef __UINT64_TYPE__  uint64_t;

/* ---- minimum-width ------------------------------------------------------ */
typedef __INT_LEAST8_TYPE__   int_least8_t;
typedef __INT_LEAST16_TYPE__  int_least16_t;
typedef __INT_LEAST32_TYPE__  int_least32_t;
typedef __INT_LEAST64_TYPE__  int_least64_t;
typedef __UINT_LEAST8_TYPE__  uint_least8_t;
typedef __UINT_LEAST16_TYPE__ uint_least16_t;
typedef __UINT_LEAST32_TYPE__ uint_least32_t;
typedef __UINT_LEAST64_TYPE__ uint_least64_t;

/* ---- fastest minimum-width ---------------------------------------------- */
typedef __INT_FAST8_TYPE__    int_fast8_t;
typedef __INT_FAST16_TYPE__   int_fast16_t;
typedef __INT_FAST32_TYPE__   int_fast32_t;
typedef __INT_FAST64_TYPE__   int_fast64_t;
typedef __UINT_FAST8_TYPE__   uint_fast8_t;
typedef __UINT_FAST16_TYPE__  uint_fast16_t;
typedef __UINT_FAST32_TYPE__  uint_fast32_t;
typedef __UINT_FAST64_TYPE__  uint_fast64_t;

/* ---- pointer-sized and greatest-width ----------------------------------- */
typedef __INTPTR_TYPE__   intptr_t;
typedef __UINTPTR_TYPE__  uintptr_t;
typedef __INTMAX_TYPE__   intmax_t;
typedef __UINTMAX_TYPE__  uintmax_t;

/* ---- limits --------------------------------------------------------------
 * The minimum of a two's-complement type is written -MAX - 1 because -MIN
 * itself does not fit the type, so it cannot be a literal. */
#define INT8_MAX    __INT8_MAX__
#define INT16_MAX   __INT16_MAX__
#define INT32_MAX   __INT32_MAX__
#define INT64_MAX   __INT64_MAX__
#define INT8_MIN    (-INT8_MAX - 1)
#define INT16_MIN   (-INT16_MAX - 1)
#define INT32_MIN   (-INT32_MAX - 1)
#define INT64_MIN   (-INT64_MAX - 1)
#define UINT8_MAX   __UINT8_MAX__
#define UINT16_MAX  __UINT16_MAX__
#define UINT32_MAX  __UINT32_MAX__
#define UINT64_MAX  __UINT64_MAX__

#define INT_LEAST8_MAX    __INT_LEAST8_MAX__
#define INT_LEAST16_MAX   __INT_LEAST16_MAX__
#define INT_LEAST32_MAX   __INT_LEAST32_MAX__
#define INT_LEAST64_MAX   __INT_LEAST64_MAX__
#define INT_LEAST8_MIN    (-INT_LEAST8_MAX - 1)
#define INT_LEAST16_MIN   (-INT_LEAST16_MAX - 1)
#define INT_LEAST32_MIN   (-INT_LEAST32_MAX - 1)
#define INT_LEAST64_MIN   (-INT_LEAST64_MAX - 1)
#define UINT_LEAST8_MAX   __UINT_LEAST8_MAX__
#define UINT_LEAST16_MAX  __UINT_LEAST16_MAX__
#define UINT_LEAST32_MAX  __UINT_LEAST32_MAX__
#define UINT_LEAST64_MAX  __UINT_LEAST64_MAX__

#define INT_FAST8_MAX     __INT_FAST8_MAX__
#define INT_FAST16_MAX    __INT_FAST16_MAX__
#define INT_FAST32_MAX    __INT_FAST32_MAX__
#define INT_FAST64_MAX    __INT_FAST64_MAX__
#define INT_FAST8_MIN     (-INT_FAST8_MAX - 1)
#define INT_FAST16_MIN    (-INT_FAST16_MAX - 1)
#define INT_FAST32_MIN    (-INT_FAST32_MAX - 1)
#define INT_FAST64_MIN    (-INT_FAST64_MAX - 1)
#define UINT_FAST8_MAX    __UINT_FAST8_MAX__
#define UINT_FAST16_MAX   __UINT_FAST16_MAX__
#define UINT_FAST32_MAX   __UINT_FAST32_MAX__
#define UINT_FAST64_MAX   __UINT_FAST64_MAX__

#define INTPTR_MAX   __INTPTR_MAX__
#define INTPTR_MIN   (-INTPTR_MAX - 1)
#define UINTPTR_MAX  __UINTPTR_MAX__
#define INTMAX_MAX   __INTMAX_MAX__
#define INTMAX_MIN   (-INTMAX_MAX - 1)
#define UINTMAX_MAX  __UINTMAX_MAX__

#define PTRDIFF_MAX  __PTRDIFF_MAX__
#define PTRDIFF_MIN  (-PTRDIFF_MAX - 1)
#define SIZE_MAX     __SIZE_MAX__
#define SIG_ATOMIC_MAX __SIG_ATOMIC_MAX__
#define SIG_ATOMIC_MIN (-SIG_ATOMIC_MAX - 1)
#define WCHAR_MAX    __WCHAR_MAX__
#define WINT_MAX     __WINT_MAX__
#ifdef __WCHAR_UNSIGNED__
#define WCHAR_MIN    0
#else
#define WCHAR_MIN    (-WCHAR_MAX - 1)
#endif
#ifdef __WINT_UNSIGNED__
#define WINT_MIN     0
#else
#define WINT_MIN     (-WINT_MAX - 1)
#endif

/* ---- integer constant macros ----------------------------------------------
 * The suffix is the target's: INT64_C(1) is 1LL where int64_t is long long
 * and 1L where it is long, and INT32_C(1) is 1L on AVR, where int32_t is long.
 * The reference compiler predefines the function-like forms for every target
 * this compiler has. */
#define INT8_C(c)    __INT8_C(c)
#define INT16_C(c)   __INT16_C(c)
#define INT32_C(c)   __INT32_C(c)
#define INT64_C(c)   __INT64_C(c)
#define UINT8_C(c)   __UINT8_C(c)
#define UINT16_C(c)  __UINT16_C(c)
#define UINT32_C(c)  __UINT32_C(c)
#define UINT64_C(c)  __UINT64_C(c)
#define INTMAX_C(c)  __INTMAX_C(c)
#define UINTMAX_C(c) __UINTMAX_C(c)

#endif /* _STDINT_H */
