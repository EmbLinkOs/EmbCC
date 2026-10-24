/* <inttypes.h> — C11 §7.8.
 *
 * Each format macro is a conversion with the length modifier of the type
 * <stdint.h> gives that name on THIS target, and that is not one answer:
 * int64_t is long on the 64-bit targets and long long on the 32-bit
 * ones; int32_t is long on AVR, Xtensa and RX and int elsewhere;
 * int_fast8_t is int on x86-64 and signed char on RISC-V. A macro
 * spelled for one is undefined behaviour on the others -- PRId64 was
 * "ld" everywhere, and on a Cortex-M printed half of every int64_t.
 *
 * So the modifiers come from the compiler's own names for the types, as
 * newlib's do: with the type keywords defined for a moment as numbers,
 * `long long int` is 4 + 4 + 2, `long int` 4 + 2, `short` 1, and #if
 * tells them apart -- which sizes cannot, int and long being four bytes
 * each on most of these targets. The keywords are given back after.
 */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma push_macro("signed")
#pragma push_macro("unsigned")
#pragma push_macro("char")
#pragma push_macro("short")
#pragma push_macro("int")
#pragma push_macro("long")
#undef signed
#undef unsigned
#undef char
#undef short
#undef int
#undef long
#define signed +0
#define unsigned +0
#define char +0
#define short +1
#define int +2
#define long +4

/* hh for a char, h for a short, nothing for an int, l for a long, ll for
 * a long long */
#if __INT8_TYPE__ == 0
#define __PRI8 "hh"
#elif __INT8_TYPE__ == 1 || __INT8_TYPE__ == 3
#define __PRI8 "h"
#elif __INT8_TYPE__ == 2
#define __PRI8 ""
#elif __INT8_TYPE__ == 4 || __INT8_TYPE__ == 6
#define __PRI8 "l"
#else
#define __PRI8 "ll"
#endif
#if __INT16_TYPE__ == 0
#define __PRI16 "hh"
#elif __INT16_TYPE__ == 1 || __INT16_TYPE__ == 3
#define __PRI16 "h"
#elif __INT16_TYPE__ == 2
#define __PRI16 ""
#elif __INT16_TYPE__ == 4 || __INT16_TYPE__ == 6
#define __PRI16 "l"
#else
#define __PRI16 "ll"
#endif
#if __INT32_TYPE__ == 0
#define __PRI32 "hh"
#elif __INT32_TYPE__ == 1 || __INT32_TYPE__ == 3
#define __PRI32 "h"
#elif __INT32_TYPE__ == 2
#define __PRI32 ""
#elif __INT32_TYPE__ == 4 || __INT32_TYPE__ == 6
#define __PRI32 "l"
#else
#define __PRI32 "ll"
#endif
#if __INT64_TYPE__ == 0
#define __PRI64 "hh"
#elif __INT64_TYPE__ == 1 || __INT64_TYPE__ == 3
#define __PRI64 "h"
#elif __INT64_TYPE__ == 2
#define __PRI64 ""
#elif __INT64_TYPE__ == 4 || __INT64_TYPE__ == 6
#define __PRI64 "l"
#else
#define __PRI64 "ll"
#endif
#if __INT_LEAST8_TYPE__ == 0
#define __PRILEAST8 "hh"
#elif __INT_LEAST8_TYPE__ == 1 || __INT_LEAST8_TYPE__ == 3
#define __PRILEAST8 "h"
#elif __INT_LEAST8_TYPE__ == 2
#define __PRILEAST8 ""
#elif __INT_LEAST8_TYPE__ == 4 || __INT_LEAST8_TYPE__ == 6
#define __PRILEAST8 "l"
#else
#define __PRILEAST8 "ll"
#endif
#if __INT_LEAST16_TYPE__ == 0
#define __PRILEAST16 "hh"
#elif __INT_LEAST16_TYPE__ == 1 || __INT_LEAST16_TYPE__ == 3
#define __PRILEAST16 "h"
#elif __INT_LEAST16_TYPE__ == 2
#define __PRILEAST16 ""
#elif __INT_LEAST16_TYPE__ == 4 || __INT_LEAST16_TYPE__ == 6
#define __PRILEAST16 "l"
#else
#define __PRILEAST16 "ll"
#endif
#if __INT_LEAST32_TYPE__ == 0
#define __PRILEAST32 "hh"
#elif __INT_LEAST32_TYPE__ == 1 || __INT_LEAST32_TYPE__ == 3
#define __PRILEAST32 "h"
#elif __INT_LEAST32_TYPE__ == 2
#define __PRILEAST32 ""
#elif __INT_LEAST32_TYPE__ == 4 || __INT_LEAST32_TYPE__ == 6
#define __PRILEAST32 "l"
#else
#define __PRILEAST32 "ll"
#endif
#if __INT_LEAST64_TYPE__ == 0
#define __PRILEAST64 "hh"
#elif __INT_LEAST64_TYPE__ == 1 || __INT_LEAST64_TYPE__ == 3
#define __PRILEAST64 "h"
#elif __INT_LEAST64_TYPE__ == 2
#define __PRILEAST64 ""
#elif __INT_LEAST64_TYPE__ == 4 || __INT_LEAST64_TYPE__ == 6
#define __PRILEAST64 "l"
#else
#define __PRILEAST64 "ll"
#endif
#if __INT_FAST8_TYPE__ == 0
#define __PRIFAST8 "hh"
#elif __INT_FAST8_TYPE__ == 1 || __INT_FAST8_TYPE__ == 3
#define __PRIFAST8 "h"
#elif __INT_FAST8_TYPE__ == 2
#define __PRIFAST8 ""
#elif __INT_FAST8_TYPE__ == 4 || __INT_FAST8_TYPE__ == 6
#define __PRIFAST8 "l"
#else
#define __PRIFAST8 "ll"
#endif
#if __INT_FAST16_TYPE__ == 0
#define __PRIFAST16 "hh"
#elif __INT_FAST16_TYPE__ == 1 || __INT_FAST16_TYPE__ == 3
#define __PRIFAST16 "h"
#elif __INT_FAST16_TYPE__ == 2
#define __PRIFAST16 ""
#elif __INT_FAST16_TYPE__ == 4 || __INT_FAST16_TYPE__ == 6
#define __PRIFAST16 "l"
#else
#define __PRIFAST16 "ll"
#endif
#if __INT_FAST32_TYPE__ == 0
#define __PRIFAST32 "hh"
#elif __INT_FAST32_TYPE__ == 1 || __INT_FAST32_TYPE__ == 3
#define __PRIFAST32 "h"
#elif __INT_FAST32_TYPE__ == 2
#define __PRIFAST32 ""
#elif __INT_FAST32_TYPE__ == 4 || __INT_FAST32_TYPE__ == 6
#define __PRIFAST32 "l"
#else
#define __PRIFAST32 "ll"
#endif
#if __INT_FAST64_TYPE__ == 0
#define __PRIFAST64 "hh"
#elif __INT_FAST64_TYPE__ == 1 || __INT_FAST64_TYPE__ == 3
#define __PRIFAST64 "h"
#elif __INT_FAST64_TYPE__ == 2
#define __PRIFAST64 ""
#elif __INT_FAST64_TYPE__ == 4 || __INT_FAST64_TYPE__ == 6
#define __PRIFAST64 "l"
#else
#define __PRIFAST64 "ll"
#endif
#if __INTMAX_TYPE__ == 0
#define __PRIMAX "hh"
#elif __INTMAX_TYPE__ == 1 || __INTMAX_TYPE__ == 3
#define __PRIMAX "h"
#elif __INTMAX_TYPE__ == 2
#define __PRIMAX ""
#elif __INTMAX_TYPE__ == 4 || __INTMAX_TYPE__ == 6
#define __PRIMAX "l"
#else
#define __PRIMAX "ll"
#endif
#if __INTPTR_TYPE__ == 0
#define __PRIPTR "hh"
#elif __INTPTR_TYPE__ == 1 || __INTPTR_TYPE__ == 3
#define __PRIPTR "h"
#elif __INTPTR_TYPE__ == 2
#define __PRIPTR ""
#elif __INTPTR_TYPE__ == 4 || __INTPTR_TYPE__ == 6
#define __PRIPTR "l"
#else
#define __PRIPTR "ll"
#endif

#undef signed
#undef unsigned
#undef char
#undef short
#undef int
#undef long
#pragma pop_macro("signed")
#pragma pop_macro("unsigned")
#pragma pop_macro("char")
#pragma pop_macro("short")
#pragma pop_macro("int")
#pragma pop_macro("long")

#define PRId8 __PRI8 "d"
#define PRId16 __PRI16 "d"
#define PRId32 __PRI32 "d"
#define PRId64 __PRI64 "d"
#define PRIdLEAST8 __PRILEAST8 "d"
#define PRIdLEAST16 __PRILEAST16 "d"
#define PRIdLEAST32 __PRILEAST32 "d"
#define PRIdLEAST64 __PRILEAST64 "d"
#define PRIdFAST8 __PRIFAST8 "d"
#define PRIdFAST16 __PRIFAST16 "d"
#define PRIdFAST32 __PRIFAST32 "d"
#define PRIdFAST64 __PRIFAST64 "d"
#define PRIdMAX __PRIMAX "d"
#define PRIdPTR __PRIPTR "d"

#define PRIi8 __PRI8 "i"
#define PRIi16 __PRI16 "i"
#define PRIi32 __PRI32 "i"
#define PRIi64 __PRI64 "i"
#define PRIiLEAST8 __PRILEAST8 "i"
#define PRIiLEAST16 __PRILEAST16 "i"
#define PRIiLEAST32 __PRILEAST32 "i"
#define PRIiLEAST64 __PRILEAST64 "i"
#define PRIiFAST8 __PRIFAST8 "i"
#define PRIiFAST16 __PRIFAST16 "i"
#define PRIiFAST32 __PRIFAST32 "i"
#define PRIiFAST64 __PRIFAST64 "i"
#define PRIiMAX __PRIMAX "i"
#define PRIiPTR __PRIPTR "i"

#define PRIo8 __PRI8 "o"
#define PRIo16 __PRI16 "o"
#define PRIo32 __PRI32 "o"
#define PRIo64 __PRI64 "o"
#define PRIoLEAST8 __PRILEAST8 "o"
#define PRIoLEAST16 __PRILEAST16 "o"
#define PRIoLEAST32 __PRILEAST32 "o"
#define PRIoLEAST64 __PRILEAST64 "o"
#define PRIoFAST8 __PRIFAST8 "o"
#define PRIoFAST16 __PRIFAST16 "o"
#define PRIoFAST32 __PRIFAST32 "o"
#define PRIoFAST64 __PRIFAST64 "o"
#define PRIoMAX __PRIMAX "o"
#define PRIoPTR __PRIPTR "o"

#define PRIu8 __PRI8 "u"
#define PRIu16 __PRI16 "u"
#define PRIu32 __PRI32 "u"
#define PRIu64 __PRI64 "u"
#define PRIuLEAST8 __PRILEAST8 "u"
#define PRIuLEAST16 __PRILEAST16 "u"
#define PRIuLEAST32 __PRILEAST32 "u"
#define PRIuLEAST64 __PRILEAST64 "u"
#define PRIuFAST8 __PRIFAST8 "u"
#define PRIuFAST16 __PRIFAST16 "u"
#define PRIuFAST32 __PRIFAST32 "u"
#define PRIuFAST64 __PRIFAST64 "u"
#define PRIuMAX __PRIMAX "u"
#define PRIuPTR __PRIPTR "u"

#define PRIx8 __PRI8 "x"
#define PRIx16 __PRI16 "x"
#define PRIx32 __PRI32 "x"
#define PRIx64 __PRI64 "x"
#define PRIxLEAST8 __PRILEAST8 "x"
#define PRIxLEAST16 __PRILEAST16 "x"
#define PRIxLEAST32 __PRILEAST32 "x"
#define PRIxLEAST64 __PRILEAST64 "x"
#define PRIxFAST8 __PRIFAST8 "x"
#define PRIxFAST16 __PRIFAST16 "x"
#define PRIxFAST32 __PRIFAST32 "x"
#define PRIxFAST64 __PRIFAST64 "x"
#define PRIxMAX __PRIMAX "x"
#define PRIxPTR __PRIPTR "x"

#define PRIX8 __PRI8 "X"
#define PRIX16 __PRI16 "X"
#define PRIX32 __PRI32 "X"
#define PRIX64 __PRI64 "X"
#define PRIXLEAST8 __PRILEAST8 "X"
#define PRIXLEAST16 __PRILEAST16 "X"
#define PRIXLEAST32 __PRILEAST32 "X"
#define PRIXLEAST64 __PRILEAST64 "X"
#define PRIXFAST8 __PRIFAST8 "X"
#define PRIXFAST16 __PRIFAST16 "X"
#define PRIXFAST32 __PRIFAST32 "X"
#define PRIXFAST64 __PRIFAST64 "X"
#define PRIXMAX __PRIMAX "X"
#define PRIXPTR __PRIPTR "X"

#define SCNd8 __PRI8 "d"
#define SCNd16 __PRI16 "d"
#define SCNd32 __PRI32 "d"
#define SCNd64 __PRI64 "d"
#define SCNdLEAST8 __PRILEAST8 "d"
#define SCNdLEAST16 __PRILEAST16 "d"
#define SCNdLEAST32 __PRILEAST32 "d"
#define SCNdLEAST64 __PRILEAST64 "d"
#define SCNdFAST8 __PRIFAST8 "d"
#define SCNdFAST16 __PRIFAST16 "d"
#define SCNdFAST32 __PRIFAST32 "d"
#define SCNdFAST64 __PRIFAST64 "d"
#define SCNdMAX __PRIMAX "d"
#define SCNdPTR __PRIPTR "d"

#define SCNi8 __PRI8 "i"
#define SCNi16 __PRI16 "i"
#define SCNi32 __PRI32 "i"
#define SCNi64 __PRI64 "i"
#define SCNiLEAST8 __PRILEAST8 "i"
#define SCNiLEAST16 __PRILEAST16 "i"
#define SCNiLEAST32 __PRILEAST32 "i"
#define SCNiLEAST64 __PRILEAST64 "i"
#define SCNiFAST8 __PRIFAST8 "i"
#define SCNiFAST16 __PRIFAST16 "i"
#define SCNiFAST32 __PRIFAST32 "i"
#define SCNiFAST64 __PRIFAST64 "i"
#define SCNiMAX __PRIMAX "i"
#define SCNiPTR __PRIPTR "i"

#define SCNo8 __PRI8 "o"
#define SCNo16 __PRI16 "o"
#define SCNo32 __PRI32 "o"
#define SCNo64 __PRI64 "o"
#define SCNoLEAST8 __PRILEAST8 "o"
#define SCNoLEAST16 __PRILEAST16 "o"
#define SCNoLEAST32 __PRILEAST32 "o"
#define SCNoLEAST64 __PRILEAST64 "o"
#define SCNoFAST8 __PRIFAST8 "o"
#define SCNoFAST16 __PRIFAST16 "o"
#define SCNoFAST32 __PRIFAST32 "o"
#define SCNoFAST64 __PRIFAST64 "o"
#define SCNoMAX __PRIMAX "o"
#define SCNoPTR __PRIPTR "o"

#define SCNu8 __PRI8 "u"
#define SCNu16 __PRI16 "u"
#define SCNu32 __PRI32 "u"
#define SCNu64 __PRI64 "u"
#define SCNuLEAST8 __PRILEAST8 "u"
#define SCNuLEAST16 __PRILEAST16 "u"
#define SCNuLEAST32 __PRILEAST32 "u"
#define SCNuLEAST64 __PRILEAST64 "u"
#define SCNuFAST8 __PRIFAST8 "u"
#define SCNuFAST16 __PRIFAST16 "u"
#define SCNuFAST32 __PRIFAST32 "u"
#define SCNuFAST64 __PRIFAST64 "u"
#define SCNuMAX __PRIMAX "u"
#define SCNuPTR __PRIPTR "u"

#define SCNx8 __PRI8 "x"
#define SCNx16 __PRI16 "x"
#define SCNx32 __PRI32 "x"
#define SCNx64 __PRI64 "x"
#define SCNxLEAST8 __PRILEAST8 "x"
#define SCNxLEAST16 __PRILEAST16 "x"
#define SCNxLEAST32 __PRILEAST32 "x"
#define SCNxLEAST64 __PRILEAST64 "x"
#define SCNxFAST8 __PRIFAST8 "x"
#define SCNxFAST16 __PRIFAST16 "x"
#define SCNxFAST32 __PRIFAST32 "x"
#define SCNxFAST64 __PRIFAST64 "x"
#define SCNxMAX __PRIMAX "x"
#define SCNxPTR __PRIPTR "x"

typedef struct { intmax_t quot; intmax_t rem; } imaxdiv_t;

intmax_t  imaxabs(intmax_t j);
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);
intmax_t  strtoimax(const char *__restrict s, char **__restrict end, int base);
uintmax_t strtoumax(const char *__restrict s, char **__restrict end, int base);

/* wchar_t is <stddef.h>'s, which this header does not bring in; the
 * compiler's name for the type is the same type. */
#ifdef __cplusplus
#define __INTTYPES_WCHAR wchar_t
#else
#define __INTTYPES_WCHAR __WCHAR_TYPE__
#endif
intmax_t  wcstoimax(const __INTTYPES_WCHAR *__restrict s,
                    __INTTYPES_WCHAR **__restrict end, int base);
uintmax_t wcstoumax(const __INTTYPES_WCHAR *__restrict s,
                    __INTTYPES_WCHAR **__restrict end, int base);
#undef __INTTYPES_WCHAR

#ifdef __cplusplus
}
#endif

#endif
