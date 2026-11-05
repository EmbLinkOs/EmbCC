/* <dlfcn.h> and <link.h>. This library links programs statically, so
 * there is one object, the program: dlopen(NULL) answers with it, a file
 * cannot be opened, and dlerror says so in glibc's words.
 *
 * dl_iterate_phdr reports that one object from the program headers the
 * kernel described in the auxiliary vector (AT_PHDR, AT_PHNUM) -- what
 * an unwinder or a profiler walking its own image reads. Where there is
 * no such vector it reports nothing.
 *
 * dladdr answers for the same object: its file, where it starts, and the
 * symbol whose bytes hold the address, read from the file's own .symtab
 * -- a static program loads no symbol table, so glibc's static dladdr
 * names none, and the frames of a sanitizer's report or of
 * backtrace_symbols were bare addresses. */
#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

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

/* The symbol of FILE's .symtab whose bytes hold A (an address as the
 * file has it): its name into NAME, its address; 0 when none. pread into
 * buffers on the stack and nothing allocated: a sanitizer's report calls
 * this with its own malloc mid-report. */
static int symtab_find(const char *file, uintptr_t a, char *name, size_t cap,
                       uintptr_t *value)
{
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int found = 0;
    ElfW(Ehdr) eh;
    if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_shentsize != sizeof(ElfW(Shdr)))
        goto done;
    ElfW(Shdr) sym, str;
    int have = 0;
    for (unsigned i = 0; i < eh.e_shnum && !have; i++)
        if (pread(fd, &sym, sizeof sym, (off_t)(eh.e_shoff + (ElfW(Off))i * sizeof sym)) ==
                (ssize_t)sizeof sym && sym.sh_type == SHT_SYMTAB &&
            sym.sh_entsize == sizeof(ElfW(Sym)) && sym.sh_link < eh.e_shnum &&
            pread(fd, &str, sizeof str, (off_t)(eh.e_shoff + (ElfW(Off))sym.sh_link * sizeof str)) ==
                (ssize_t)sizeof str)
            have = 1;
    if (!have)
        goto done;
    ElfW(Sym) buf[64], best;
    size_t total = (size_t)(sym.sh_size / sizeof(ElfW(Sym)));
    for (size_t at = 0; at < total; ) {
        size_t k = total - at < 64 ? total - at : 64;
        ssize_t got = pread(fd, buf, k * sizeof buf[0],
                            (off_t)(sym.sh_offset + at * sizeof buf[0]));
        if (got <= 0)
            break;
        k = (size_t)got / sizeof buf[0];
        for (size_t j = 0; j < k; j++) {
            const ElfW(Sym) *e = &buf[j];
            int t = e->st_info & 0xf;          /* the type: ST_TYPE */
            if ((t == STT_FUNC || t == STT_OBJECT) && e->st_shndx != SHN_UNDEF &&
                e->st_size && a >= e->st_value && a - e->st_value < e->st_size &&
                (!found || t == STT_FUNC)) {
                best = *e;
                found = 1;
            }
        }
        at += k;
    }
    if (found) {
        ssize_t n = pread(fd, name, cap - 1, (off_t)(str.sh_offset + best.st_name));
        name[n > 0 ? (size_t)n : 0] = 0;
        found = name[0] != 0;
        *value = (uintptr_t)best.st_value;
    }
done:
    close(fd);
    return found;
}

/* dladdr for the one object: 0 for an address outside its loaded
 * segments. The name is in a static buffer, good until the next call. */
WEAK int dladdr(const void *addr, Dl_info *info)
{
    static char sname[160];
    memset(info, 0, sizeof *info);
    const long *a = __os_auxv();
    const ElfW(Phdr) *ph = 0;
    unsigned long n = 0;
    const char *file = 0;
    uintptr_t page = 4096;
    for (; a && a[0]; a += 2) {
        if (a[0] == AT_PHDR)
            ph = (const ElfW(Phdr) *)(uintptr_t)a[1];
        else if (a[0] == AT_PHNUM)
            n = (unsigned long)a[1];
        else if (a[0] == AT_EXECFN)
            file = (const char *)(uintptr_t)a[1];
        else if (a[0] == AT_PAGESZ)
            page = (uintptr_t)a[1];
    }
    if (!ph || !n)
        return 0;
    uintptr_t bias = 0, base = 0, p = (uintptr_t)addr;
    for (unsigned long i = 0; i < n; i++)
        if (ph[i].p_type == PT_PHDR)
            bias = (uintptr_t)ph - ph[i].p_vaddr;
    int in = 0, have_base = 0;
    for (unsigned long i = 0; i < n; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;
        uintptr_t lo = bias + ph[i].p_vaddr;
        if (!have_base) {               /* the first segment's page, as glibc's */
            base = lo & ~(page - 1);
            have_base = 1;
        }
        if (p >= lo && p - lo < ph[i].p_memsz)
            in = 1;
    }
    if (!in)
        return 0;
    info->dli_fname = file ? file : "";
    info->dli_fbase = (void *)base;
    uintptr_t v;
    if (file && symtab_find(file, p - bias, sname, sizeof sname, &v)) {
        info->dli_sname = sname;
        info->dli_saddr = (void *)(v + bias);
    }
    return 1;
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
