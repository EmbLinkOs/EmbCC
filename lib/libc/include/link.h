/* <link.h> -- walking the loaded objects' program headers
 * (src/posix/dl.c). A statically linked program is one object. */
#ifndef _LINK_H
#define _LINK_H

#include <elf.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if __SIZEOF_POINTER__ == 8
#define ElfW(type) Elf64_##type
#else
#define ElfW(type) Elf32_##type
#endif

struct dl_phdr_info {
    ElfW(Addr) dlpi_addr;          /* where it was loaded, less where linked */
    const char *dlpi_name;
    const ElfW(Phdr) *dlpi_phdr;
    ElfW(Half) dlpi_phnum;
    unsigned long long dlpi_adds;  /* objects loaded, and unloaded, so far */
    unsigned long long dlpi_subs;
    size_t dlpi_tls_modid;
    void *dlpi_tls_data;
};

int dl_iterate_phdr(int (*fn)(struct dl_phdr_info *info, size_t size, void *data),
                    void *data);

#ifdef __cplusplus
}
#endif

#endif
