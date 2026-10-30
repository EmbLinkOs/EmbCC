/* locale_t, for every header that has an _l function: POSIX makes it
 * visible there without <locale.h>. */
#ifndef _BITS_LOCALE_T_H
#define _BITS_LOCALE_T_H
typedef struct __locale_struct *locale_t;
#endif
