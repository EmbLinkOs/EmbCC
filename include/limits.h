/* EmbCC's limits.h: the compiler's half, as GCC's is — newlib's own
 * limits.h reaches it with #include_next when __GNUC__ is defined (C++
 * units); found first, it reaches newlib's the same way. Values from the
 * target's predefined macros. */
#ifndef _GCC_LIMITS_H_
#define _GCC_LIMITS_H_

/* found first (EmbCC's include directory ahead of the system's, as the
 * OS build has it): the C library's limits.h too — its POSIX limits
 * (PATH_MAX, ...) — which then does not come back here */
#ifndef _LIBC_LIMITS_H_
#include_next <limits.h>
#endif

#ifndef CHAR_BIT
#define CHAR_BIT __CHAR_BIT__
#endif

#ifndef MB_LEN_MAX
#define MB_LEN_MAX 1
#endif

#ifndef SCHAR_MIN
#define SCHAR_MIN (-SCHAR_MAX - 1)
#endif
#ifndef SCHAR_MAX
#define SCHAR_MAX __SCHAR_MAX__
#endif
#ifndef UCHAR_MAX
#define UCHAR_MAX (SCHAR_MAX * 2 + 1)
#endif

#ifndef CHAR_MIN
#ifdef __CHAR_UNSIGNED__
#define CHAR_MIN 0
#define CHAR_MAX UCHAR_MAX
#else
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#endif
#endif

#ifndef SHRT_MIN
#define SHRT_MIN (-SHRT_MAX - 1)
#endif
#ifndef SHRT_MAX
#define SHRT_MAX __SHRT_MAX__
#endif
#ifndef USHRT_MAX
#define USHRT_MAX (SHRT_MAX * 2 + 1)
#endif

#ifndef INT_MIN
#define INT_MIN (-INT_MAX - 1)
#endif
#ifndef INT_MAX
#define INT_MAX __INT_MAX__
#endif
#ifndef UINT_MAX
#define UINT_MAX (INT_MAX * 2U + 1U)
#endif

#ifndef LONG_MIN
#define LONG_MIN (-LONG_MAX - 1L)
#endif
#ifndef LONG_MAX
#define LONG_MAX __LONG_MAX__
#endif
#ifndef ULONG_MAX
#define ULONG_MAX (LONG_MAX * 2UL + 1UL)
#endif

#ifndef LLONG_MIN
#define LLONG_MIN (-LLONG_MAX - 1LL)
#endif
#ifndef LLONG_MAX
#define LLONG_MAX __LONG_LONG_MAX__
#endif
#ifndef ULLONG_MAX
#define ULLONG_MAX (LLONG_MAX * 2ULL + 1ULL)
#endif

#ifndef LONG_LONG_MIN
#define LONG_LONG_MIN (-LONG_LONG_MAX - 1LL)
#endif
#ifndef LONG_LONG_MAX
#define LONG_LONG_MAX __LONG_LONG_MAX__
#endif
#ifndef ULONG_LONG_MAX
#define ULONG_LONG_MAX (LONG_LONG_MAX * 2ULL + 1ULL)
#endif

/* C23: each integer type's width, and the widest _BitInt */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ > 201710L && \
    !defined(BITINT_MAXWIDTH)
#define BOOL_WIDTH 1
#define CHAR_WIDTH __CHAR_BIT__
#define SCHAR_WIDTH __CHAR_BIT__
#define UCHAR_WIDTH __CHAR_BIT__
#define SHRT_WIDTH (__SIZEOF_SHORT__ * __CHAR_BIT__)
#define USHRT_WIDTH SHRT_WIDTH
#define INT_WIDTH (__SIZEOF_INT__ * __CHAR_BIT__)
#define UINT_WIDTH INT_WIDTH
#define LONG_WIDTH (__SIZEOF_LONG__ * __CHAR_BIT__)
#define ULONG_WIDTH LONG_WIDTH
#define LLONG_WIDTH (__SIZEOF_LONG_LONG__ * __CHAR_BIT__)
#define ULLONG_WIDTH LLONG_WIDTH
#define BITINT_MAXWIDTH __BITINT_MAXWIDTH__
#endif

#endif /* _GCC_LIMITS_H_ */
