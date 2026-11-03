/* backtrace: the return addresses up the stack. Where the program has
 * EmbCC's unwinder (Linux, and EmbLinkOS, whose librt.a carries it) the
 * walk is the unwinder's, over .eh_frame -- what glibc's does through
 * libgcc -- since x86-64 code leaves out frame pointers where it can.
 * Elsewhere, AArch64's frame records are a chain EmbCC always keeps, and
 * startup ends it with a null one; a pointer that does not move up the
 * stack ends the walk rather than being followed. Anywhere else there is
 * no chain, and the caller's return address is all there is.
 *
 * The text is glibc's: "file(symbol+0xoff) [address]" where dladdr
 * knows the address, "[address]" where it does not -- a static program's
 * symbols are not loaded, as they are not there. */
#include <dlfcn.h>
#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posix.h"

#if defined(__linux__) || defined(__EMBLIBC_UNWINDER)
#define UNWINDER 1
struct _Unwind_Context;
int _Unwind_Backtrace(int (*fn)(struct _Unwind_Context *, void *), void *arg);
unsigned long _Unwind_GetIP(struct _Unwind_Context *c);

struct walk { void **buf; int size, n; };

static int one_frame(struct _Unwind_Context *c, void *arg)
{
    struct walk *w = arg;
    if (w->n < 0) {                    /* backtrace's own frame */
        w->n = 0;
        return 0;
    }
    if (w->n >= w->size)
        return 5;                      /* _URC_END_OF_STACK: enough */
    w->buf[w->n++] = (void *)_Unwind_GetIP(c);
    return 0;
}
#endif

WEAK int backtrace(void **buffer, int size)
{
    if (size <= 0)
        return 0;
#if defined(UNWINDER)
    struct walk w = { buffer, size, -1 };
    _Unwind_Backtrace(one_frame, &w);
    if (w.n > 0 && !buffer[w.n - 1])
        w.n--;                         /* the outermost frame's null */
    return w.n;
#elif defined(__aarch64__)
    void **fp = __builtin_frame_address(0);
    int n = 0;
    while (fp && n < size) {
        void *ret = fp[1];
        if (!ret)
            break;
        buffer[n++] = ret;
        void **up = fp[0];
        if (up <= fp || ((uintptr_t)up & (sizeof(void *) - 1)) ||
            (uintptr_t)up - (uintptr_t)fp > (16u << 20))
            break;
        fp = up;
    }
    return n;
#else
    buffer[0] = __builtin_return_address(0);
    return 1;
#endif
}

/* one line, into out (when not null): its length without the NUL */
static int describe(void *addr, char *out, size_t cap)
{
    Dl_info di;
    if (dladdr(addr, &di) && di.dli_fname && *di.dli_fname) {
        if (di.dli_sname && di.dli_saddr)
            return snprintf(out, cap, "%s(%s+%#tx) [%p]", di.dli_fname, di.dli_sname,
                            (char *)addr - (char *)di.dli_saddr, addr);
        return snprintf(out, cap, "%s(+%#tx) [%p]", di.dli_fname,
                        (char *)addr - (char *)di.dli_fbase, addr);
    }
    return snprintf(out, cap, "[%p]", addr);
}

/* one malloc'd block: the pointers, then the strings they point at */
WEAK char **backtrace_symbols(void *const *buffer, int size)
{
    if (size < 0)
        size = 0;
    size_t bytes = (size_t)size * sizeof(char *);
    for (int i = 0; i < size; i++)
        bytes += (size_t)describe(buffer[i], 0, 0) + 1;
    char **v = malloc(bytes ? bytes : 1);
    if (!v)
        return 0;
    char *s = (char *)(v + size), *end = (char *)v + bytes;
    for (int i = 0; i < size; i++) {
        v[i] = s;
        s += describe(buffer[i], s, (size_t)(end - s)) + 1;
    }
    return v;
}

WEAK void backtrace_symbols_fd(void *const *buffer, int size, int fd)
{
    char line[512];
    for (int i = 0; i < size; i++) {
        int n = describe(buffer[i], line, sizeof line - 1);
        if (n < 0)
            continue;
        if ((size_t)n > sizeof line - 2)
            n = sizeof line - 2;
        line[n++] = '\n';
        write(fd, line, (size_t)n);
    }
}
