/* loader.c -- the ELF image: every PT_LOAD segment at its physical
 * (load) address, as QEMU's -kernel and a flash programmer place it (a
 * MIPS image's, which is in kseg0 or kseg1, at its physical address). A
 * segment outside the board's memory gets a region of its own, so a link
 * script for a slightly different part still runs; one at a device's
 * registers is written to them. ELFCLASS32, and
 * ELFCLASS64 for a core that takes it (RV64); the entry point, the class
 * and e_flags go into struct sim for the core's reset. An Intel HEX
 * file (a first character ':') is read as records at bus addresses. */
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

/* bytes for the bus at pa: memory, or a device's registers */
static void put(struct sim *s, const char *path, u32 pa, const u8 *b, u32 fsz)
{
    u32 x;
    if (!bus_region(&s->bus, pa, 1) && !bus_debug_read(&s->bus, pa, 1, &x)) {
        /* a segment for a device's registers (XC32 puts a PIC32's OFFn
         * table in one): written to them, as a debugger's writes go */
        for (u32 k = 0; k < fsz; k += 4) {
            u32 v = 0;
            int n = fsz - k >= 4 ? 4 : 1;
            for (int i = 0; i < n; i++)
                v |= (u32)b[k + (u32)i] << (8 * i);
            if (n == 4 && !((pa + k) & 3))
                bus_debug_write(&s->bus, pa + k, 4, v);
            else
                for (u32 i = k; i < fsz && i < k + 4; i++)
                    bus_debug_write(&s->bus, pa + i, 1, b[i]);
        }
        return;
    }
    if (!bus_region(&s->bus, pa, fsz)) {
        u32 base = pa & ~0xfffu, end = (pa + fsz + 0xfffu) & ~0xfffu;
        bus_add_region(&s->bus, base, end - base, MEM_RAM);
    }
    for (u32 k = 0; k < fsz; k++) {
        struct region *r = bus_region(&s->bus, pa + k, 1);
        if (!r)
            die("%s: a segment crosses 0x%08x, outside memory", path, pa + k);
        r->mem[pa + k - r->base] = b[k];
    }
}

static void load_hex(struct sim *s, const char *path, const u8 *f, size_t len);

void load_elf(struct sim *s, const char *path)
{
    size_t len;
    u8 *f = slurp(path, &len);
    size_t sp = 0;
    while (sp < len && (f[sp] == ' ' || f[sp] == '\r' || f[sp] == '\n'))
        sp++;
    if (sp < len && f[sp] == ':') {
        load_hex(s, path, f, len);
        free(f);
        return;
    }
    if (len < 52 || memcmp(f, "\177ELF", 4) != 0)
        die("%s is not an ELF file or an Intel HEX image", path);
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
        if (s->cpu->ops->phys)
            pa = s->cpu->ops->phys(s->cpu, pa);
        put(s, path, pa, f + off, fsz);
        loaded++;
    }
    free(f);
    if (!loaded)
        die("%s has nothing to load", path);
}

/* An Intel HEX image (MPLAB's production .hex, an objcopy -O ihex): its
 * data records at their addresses, which are the bus's -- a PIC24's
 * program word at twice its address, as XC16's hex2 lays it out; the
 * start address record, if any, the entry. */
static void load_hex(struct sim *s, const char *path, const u8 *f, size_t len)
{
    u32 base = 0;
    int loaded = 0, line = 0;
    u8 rec[260];
    for (size_t i = 0; i < len; ) {
        while (i < len && (f[i] == '\r' || f[i] == '\n' || f[i] == ' ' || f[i] == '\t'))
            i += f[i] == '\n' ? (line++, 1) : 1;
        if (i >= len)
            break;
        if (f[i] != ':')
            die("%s:%d: not an Intel HEX record", path, line + 1);
        i++;
        int n = 0;
        u8 sum = 0;
        while (i + 1 < len && n < (int)sizeof rec) {
            int hi = f[i], lo = f[i + 1], v = 0;
            if (!((hi >= '0' && hi <= '9') || (hi >= 'A' && hi <= 'F') ||
                  (hi >= 'a' && hi <= 'f')))
                break;
            for (int k = 0; k < 2; k++) {
                int c = k ? lo : hi;
                v = v * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            }
            rec[n++] = (u8)v;
            sum = (u8)(sum + v);
            i += 2;
        }
        if (n < 5 || n != rec[0] + 5 || sum)
            die("%s:%d: a bad Intel HEX record", path, line + 1);
        u32 a = (u32)rec[1] << 8 | rec[2];
        switch (rec[3]) {
        case 0:
            put(s, path, base + a, rec + 4, rec[0]);
            loaded++;
            break;
        case 1:
            i = len;
            break;
        case 2:
            base = ((u32)rec[4] << 8 | rec[5]) << 4;
            break;
        case 4:
            base = ((u32)rec[4] << 8 | rec[5]) << 16;
            break;
        case 3:
            s->entry = ((u32)rec[4] << 8 | rec[5]) << 4 | ((u32)rec[6] << 8 | rec[7]);
            break;
        case 5:
            s->entry = (u32)rec[4] << 24 | (u32)rec[5] << 16 | (u32)rec[6] << 8 | rec[7];
            break;
        }
    }
    if (!loaded)
        die("%s has nothing to load", path);
}
