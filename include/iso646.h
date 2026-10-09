/* <iso646.h> — C11 §7.9.
 *
 * Alternative spellings for the operators whose characters are not on
 * every keyboard. In C++ these are KEYWORDS, not macros, so this header
 * is empty there -- and defining them as macros would be worse than
 * useless: `and` is already a token the compiler knows, and a macro
 * would shadow it in places the standard says it must not be shadowed.
 */
#ifndef _ISO646_H
#define _ISO646_H

#ifndef __cplusplus
#define and    &&
#define and_eq &=
#define bitand &
#define bitor  |
#define compl  ~
#define not    !
#define not_eq !=
#define or     ||
#define or_eq  |=
#define xor    ^
#define xor_eq ^=
#endif

#endif
