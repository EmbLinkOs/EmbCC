/* EmbCC's <stddef.h> for a Darwin target: Apple's, with C23's additions,
 * which the SDK leaves to the compiler's own stddef.h (clang's is searched
 * before it). This directory is searched before the SDK's, for Darwin
 * targets only (src/driver/main.c). */
#include_next <stddef.h>

#if !defined(__cplusplus) && defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#ifndef __EMBCC_NULLPTR_T
#define __EMBCC_NULLPTR_T
typedef typeof(nullptr) nullptr_t;
#endif
#ifndef unreachable
#define unreachable() __builtin_unreachable()
#endif
#endif
