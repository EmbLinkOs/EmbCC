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
 * harnesses and EmbLinkOS have no such step, so there errno stays one
 * variable -- which is correct for them, because they are
 * single-threaded.
 *
 * The consequence of getting this wrong is worth stating plainly: a
 * shared errno under real threads is not a slow program, it is two
 * threads reading each other's failures.
 */
#include <errno.h>

#if defined(__linux__)
#define ERRNO_STORAGE __thread
#else
#define ERRNO_STORAGE
#endif

static ERRNO_STORAGE int g_errno;

int *__errno_location(void) { return &g_errno; }

/* newlib spells the accessor `__errno`, and code built against its headers
 * -- a target's existing syscall shims, for instance -- calls that name.
 * Providing it costs one line and lets this library drop in where newlib
 * was without rebuilding everything around it. */
int *__errno(void) { return &g_errno; }
