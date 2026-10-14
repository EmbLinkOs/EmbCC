/* <stdnoreturn.h> — C11 §7.23.
 *
 * One macro. `noreturn` on a function tells the compiler the call never
 * comes back, which is not a hint: it lets the flow analysis stop
 * following the path after it, so `exit(1);` at the end of a branch is
 * understood to end the branch, and a missing `return` after it is not
 * a warning or a fall-through bug.
 */
#ifndef _STDNORETURN_H
#define _STDNORETURN_H

#ifndef __cplusplus
#define noreturn _Noreturn
#endif

#endif
