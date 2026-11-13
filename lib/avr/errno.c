/* AVR has no threads of its own, so errno is one value for the program. */
#include <errno.h>

static int g_errno;

int *__errno_location(void) { return &g_errno; }
