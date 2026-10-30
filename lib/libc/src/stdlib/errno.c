/* errno's storage: one per thread where the target has threads, one for
 * the program where it does not. Behind a function, which is what let
 * that change happen here and nowhere else (<errno.h>).
 *
 * ---- why this is conditional ----------------------------------------
 *
 * `__thread` is not a promise the COMPILER can keep on its own. It
 * emits .tdata/.tbss and addresses them off a thread pointer, but
 * something has to have allocated a block and pointed the hardware at
 * it before the first access -- and on a target whose startup does not,
 * the thread pointer is zero and the first errno read faults near
 * address 0.
 *
 * So this is thread-local exactly where a runtime sets that up. Today
 * that is Linux: lib/libc/os/linux/tls.c finds PT_TLS through the
 * auxiliary vector, gives the first thread its copy before anything
 * else runs, and gives every clone()d thread its own. The freestanding
 * harnesses have no such step and no threads, so there errno is one
 * variable. EmbLinkOS has threads but only half the step: its crt0
 * builds the first thread's block and hands the template over
 * (os/tls-template.h), and its backend copies that template into every
 * new thread before the thread runs. Until the hand-over -- the first
 * instructions of the program, or a startup that predates it -- there
 * is no block to address, and errno is the one shared variable.
 *
 * The consequence of getting this wrong is worth stating plainly: a
 * shared errno under real threads is not a slow program, it is two
 * threads reading each other's failures.
 */
#include <errno.h>

#if defined(__linux__)

static __thread int g_errno;

int *__errno_location(void) { return &g_errno; }

#elif defined(__EMBLIBC_TLS_TEMPLATE)

#include "../../os/tls-template.h"

struct __emblibc_tls __emblibc_tls;   /* startup fills it; see the header */

static __thread int t_errno;
static int g_errno;

/* One load and a branch per access, against a call to the kernel to ask
 * which thread this is. memsz goes nonzero once, before main and before
 * any second thread can exist, so no thread ever sees it change. */
int *__errno_location(void)
{
    return __emblibc_tls.memsz ? &t_errno : &g_errno;
}

#else

static int g_errno;

int *__errno_location(void) { return &g_errno; }

#endif

/* newlib spells the accessor `__errno`, and code built against its headers
 * -- a target's existing syscall shims, for instance -- calls that name.
 * Providing it costs one line and lets this library drop in where newlib
 * was without rebuilding everything around it. */
int *__errno(void) { return __errno_location(); }
