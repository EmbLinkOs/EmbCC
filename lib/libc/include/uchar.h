/* <uchar.h> — C11 §7.28.
 *
 * UTF-16 and UTF-32 conversions, with the types named so the width is
 * in the type rather than in a comment. This is the header that exists
 * because `wchar_t` does not say how wide it is: `char32_t` always
 * holds a code point and `char16_t` always holds a UTF-16 code unit, on
 * every target.
 *
 * The UTF-16 direction is where the work is. A code point above 0xFFFF
 * takes TWO code units -- a surrogate pair -- so c32rtomb's counterpart
 * has to emit one unit, return, and remember that it owes another. That
 * is what the mbstate_t is for, and it is why these cannot be one-line
 * wrappers.
 */
#ifndef _UCHAR_H
#define _UCHAR_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __cplusplus
typedef __CHAR16_TYPE__ char16_t;
typedef __CHAR32_TYPE__ char32_t;
#endif

size_t mbrtoc16(char16_t *__restrict pc16, const char *__restrict s, size_t n,
                mbstate_t *__restrict ps);
size_t c16rtomb(char *__restrict s, char16_t c16, mbstate_t *__restrict ps);
size_t mbrtoc32(char32_t *__restrict pc32, const char *__restrict s, size_t n,
                mbstate_t *__restrict ps);
size_t c32rtomb(char *__restrict s, char32_t c32, mbstate_t *__restrict ps);

#ifdef __cplusplus
}
#endif

#endif
