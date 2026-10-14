/* EmbCC's <math.h> for a Darwin target: Apple's, with what it gets wrong
 * for a compiler that does not define __GNUC__ put right.
 *
 * The SDK picks its NAN and HUGE_VAL spellings on __GNUC__. Without it NAN
 * is `__nan()` -- a function the same header declares only for i386 and
 * x86-64, so on arm64 every use of NAN was "'__nan' is not declared" --
 * and HUGE_VAL is `1e500`, a constant out of double's range. The builtins
 * are what clang's branch of the header uses. This directory is searched
 * before the SDK's, for Darwin targets only (src/driver/main.c). */
#include_next <math.h>

#ifndef __GNUC__
#undef NAN
#define NAN       __builtin_nanf("")
#undef INFINITY
#define INFINITY  __builtin_inff()
#undef HUGE_VAL
#define HUGE_VAL  __builtin_huge_val()
#undef HUGE_VALF
#define HUGE_VALF __builtin_huge_valf()
#undef HUGE_VALL
#define HUGE_VALL __builtin_huge_vall()
#endif
