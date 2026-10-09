/* loader.c -- the ELF image: every PT_LOAD segment at its physical
 * (load) address, as QEMU's -kernel and a flash programmer place it. A
 * segment outside the board's memory gets a region of its own, so a link
 * script for a slightly different part still runs. ELFCLASS32, and
 * ELFCLASS64 for a core that takes it (RV64); the entry point, the class
 * and e_flags go into struct sim for the core's reset. */
#include <stdlib.h>
#include <string.h>

#include "sim.h"

static u8 *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open %s", path);
    size_t cap = 1 << 16, n = 0;
    u8 *buf = malloc(cap);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        if (!buf)
            die("out of memory reading %s", path);
        size_t got = fread(buf + n, 1, cap - n, f);
        if (!got)
            break;
        n += got;
    }
    fclose(f);
    *len = n;
    return buf;
}

static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return le16(p) | le16(p + 2) << 16; }
static u64 le64(const u8 *p) { return le32(p) | (u64)le32(p + 4) << 32; }

void load_elf(struct sim *s, const char *path)
{
    size_t len;
    u8 *f = slurp(path, &len);
    if (len < 52 || memcmp(f, "\177ELF", 4) != 0)
        die("%s is not an ELF file", path);
    int e64 = f[4] == 2;
    if (!(f[4] == 1 || (e64 && (s->cpu->ops->elf_classes & 2) && len >= 64)) ||
        f[5] != 1)
        die("%s is not a %s little-endian ELF image", path,
            (s->cpu->ops->elf_classes & 2) ? "32- or 64-bit" : "32-bit");
    if (le16(f + 18) != (u32)s->cpu->ops->elf_machine)
        die("%s is not %s (e_machine %u)", path, s->cpu->ops->elf_name,
            le16(f + 18));
    if (le16(f + 16) != 2)
        die("%s is not a linked image (link it first)", path);
    if (!e64 && !(s->cpu->ops->elf_classes & 1))
        die("%s is a 32-bit image", path);
    u64 phoff = e64 ? le64(f + 32) : le32(f + 28);
    u32 phentsize = le16(f + (e64 ? 54 : 42));
    u32 phnum = le16(f + (e64 ? 56 : 44));
    s->entry = e64 ? le64(f + 24) : le32(f + 24);
    s->elf64 = e64;
    s->elf_flags = le32(f + (e64 ? 48 : 36));
    int loaded = 0;
    for (u32 i = 0; i < phnum; i++) {
        const u8 *ph = f + phoff + (u64)i * phentsize;
        if (phoff + (u64)i * phentsize + (e64 ? 56 : 32) > len)
            die("%s: truncated program headers", path);
        if (le32(ph) != 1)
            continue;
        u64 off = e64 ? le64(ph + 8) : le32(ph + 4);
        u64 pa64 = e64 ? le64(ph + 24) : le32(ph + 12);
        u64 fsz64 = e64 ? le64(ph + 32) : le32(ph + 16);
        if (!fsz64)
            continue;
        if (off + fsz64 > len)
            die("%s: a segment runs past the end of the file", path);
        if (pa64 + fsz64 - 1 > 0xffffffffu)
            die("%s: a segment at 0x%llx is beyond the 32-bit bus", path,
                (unsigned long long)pa64);
        u32 pa = (u32)pa64, fsz = (u32)fsz64;
        if (!bus_region(&s->bus, pa, fsz)) {
            u32 base = pa & ~0xfffu, end = (pa + fsz + 0xfffu) & ~0xfffu;
            bus_add_region(&s->bus, base, end - base, MEM_RAM);
        }
        for (u32 k = 0; k < fsz; k++) {
            struct region *r = bus_region(&s->bus, pa + k, 1);
            if (!r)
                die("%s: a segment crosses 0x%08x, outside memory", path,
                    pa + k);
            r->mem[pa + k - r->base] = f[off + k];
        }
        loaded++;
    }
    free(f);
    if (!loaded)
        die("%s has nothing to load", path);
}
