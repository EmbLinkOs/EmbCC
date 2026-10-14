/* <stdalign.h> — C11 §7.15.
 *
 * `alignas` and `alignof` as macros over the underscore-prefixed
 * keywords. The keywords are what the language has; this header is how
 * C code written before C11 -- or written to be readable -- reaches
 * them. In C++ both are keywords already, so the header defines only
 * the two feature macros the standard asks for.
 */
#ifndef _STDALIGN_H
#define _STDALIGN_H

#ifndef __cplusplus
#define alignas _Alignas
#define alignof _Alignof
#endif

#define __alignas_is_defined 1
#define __alignof_is_defined 1

#endif
