/* loader.c -- the ELF image: every PT_LOAD segment at its physical
 * (load) address, as QEMU's -kernel and a flash programmer place it. A
 * segment outside the board's memory gets a region of its own, so a link
 * script for a slightly different part still runs. */
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

void load_elf(struct sim *s, const char *path)
{
    size_t len;
    u8 *f = slurp(path, &len);
    if (len < 52 || memcmp(f, "\177ELF", 4) != 0)
        die("%s is not an ELF file", path);
    if (f[4] != 1 || f[5] != 1)
        die("%s is not a 32-bit little-endian ELF image", path);
    if (le16(f + 18) != (u32)s->cpu->ops->elf_machine)
        die("%s is not %s (e_machine %u)", path, s->cpu->ops->elf_name,
            le16(f + 18));
    if (le16(f + 16) != 2)
        die("%s is not a linked image (link it first)", path);
    u32 phoff = le32(f + 28), phentsize = le16(f + 42), phnum = le16(f + 44);
    int loaded = 0;
    for (u32 i = 0; i < phnum; i++) {
        const u8 *ph = f + phoff + i * phentsize;
        if ((size_t)(ph - f) + 32 > len)
            die("%s: truncated program headers", path);
        if (le32(ph) != 1)
            continue;
        u32 off = le32(ph + 4), pa = le32(ph + 12);
        u32 fsz = le32(ph + 16);
        if (!fsz)
            continue;
        if ((size_t)off + fsz > len)
            die("%s: a segment runs past the end of the file", path);
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
