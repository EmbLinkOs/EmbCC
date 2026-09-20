/* errno's storage. One per thread when there are threads; one, here, when
 * there are not. Behind a function so that adding threads later changes
 * this file and nothing else (<errno.h>). */
#include <errno.h>

static int g_errno;

int *__errno_location(void) { return &g_errno; }

/* newlib spells the accessor `__errno`, and code built against its headers
 * -- a target's existing syscall shims, for instance -- calls that name.
 * Providing it costs one line and lets this library drop in where newlib
 * was without rebuilding everything around it. */
int *__errno(void) { return &g_errno; }
