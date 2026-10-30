/* <dlfcn.h> and <link.h>. This library links programs statically, so
 * there is one object, the program: dlopen(NULL) answers with it, a file
 * cannot be opened, and dlerror says so in glibc's words.
 *
 * dl_iterate_phdr reports that one object from the program headers the
 * kernel described in the auxiliary vector (AT_PHDR, AT_PHNUM) -- what
 * an unwinder or a profiler walking its own image reads. Where there is
 * no such vector it reports nothing. */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>

#include "posix.h"

static int g_self;
static char g_err[256];
static int g_pending;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    g_pending = 1;
}

WEAK void *dlopen(const char *file, int mode)
{
    (void)mode;
    if (!file)
        return &g_self;
    fail("%s: cannot open shared object file: this program is linked statically",
         file);
    return 0;
}

WEAK int dlclose(void *handle)
{
    if (handle == &g_self)
        return 0;
    fail("shared object not open");
    return -1;
}

WEAK void *dlsym(void *restrict handle, const char *restrict name)
{
    (void)handle;
    fail("undefined symbol: %s", name);
    return 0;
}

WEAK void *dlvsym(void *restrict handle, const char *restrict name,
                  const char *restrict version)
{
    (void)version;
    return dlsym(handle, name);
}

/* the message once, then NULL until the next failure */
WEAK char *dlerror(void)
{
    if (!g_pending)
        return 0;
    g_pending = 0;
    return g_err;
}

WEAK int dladdr(const void *addr, Dl_info *info)
{
    (void)addr;
    memset(info, 0, sizeof *info);
    return 0;
}

WEAK int dlinfo(void *restrict handle, int request, void *restrict arg)
{
    (void)handle;
    (void)arg;
    fail("dlinfo: request %d not supported", request);
    return -1;
}

WEAK int dl_iterate_phdr(int (*fn)(struct dl_phdr_info *, size_t, void *),
                         void *data)
{
    const long *a = __os_auxv();
    const ElfW(Phdr) *ph = 0;
    unsigned long n = 0;
    for (; a && a[0]; a += 2) {
        if (a[0] == AT_PHDR)
            ph = (const ElfW(Phdr) *)(uintptr_t)a[1];
        else if (a[0] == AT_PHNUM)
            n = (unsigned long)a[1];
    }
    if (!ph || !n)
        return 0;
    struct dl_phdr_info info;
    memset(&info, 0, sizeof info);
    info.dlpi_name = "";
    info.dlpi_phdr = ph;
    info.dlpi_phnum = (ElfW(Half))n;
    info.dlpi_adds = 1;
    /* the load bias: where PT_PHDR is, less where it says it is */
    for (unsigned long i = 0; i < n; i++) {
        if (ph[i].p_type == PT_PHDR)
            info.dlpi_addr = (ElfW(Addr))((uintptr_t)ph - ph[i].p_vaddr);
        if (ph[i].p_type == PT_TLS)
            info.dlpi_tls_modid = 1;
    }
    return fn(&info, sizeof info, data);
}
