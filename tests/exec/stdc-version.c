/* EmbCC compiles C11's language (C17 is C11 with its defects fixed), and
 * says so: __STDC_VERSION__ was 199901L, which sent headers to their
 * pre-C11 fallbacks and kept C11's own macros out of <float.h>. */
// expect-exit: 42
#include <float.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "the compiler does not claim C11"
#endif
#ifndef FLT_TRUE_MIN
#error "<float.h> lacks C11's FLT_TRUE_MIN"
#endif
int main(void)
{
    return FLT_TRUE_MIN > 0 && DBL_DECIMAL_DIG >= 17 && DBL_HAS_SUBNORM ? 42 : 1;
}
