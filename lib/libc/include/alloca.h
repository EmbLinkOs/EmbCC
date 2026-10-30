/* <alloca.h> -- stack allocation, which the compiler does itself */
#ifndef _ALLOCA_H
#define _ALLOCA_H

#include <stddef.h>

#define alloca(n) __builtin_alloca(n)

#endif
