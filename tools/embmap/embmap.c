/* embmap -- where a firmware image's memory goes.
 *
 *   embmap IMAGE [--region NAME:ORIGIN:LENGTH]... [--top N]
 *                [--map FILE.map] [--diff OLD] [--max-flash N] [--max-ram N]
 *                [--json]
 *
 * It reads a linked ELF image -- EmbLD's, GNU ld's or lld's, 32- or 64-bit,
 * either byte order, any machine -- and answers the questions a firmware
 * developer asks when the part fills up:
 *
 *   - how much FLASH and how much RAM the image takes, section by section,
 *     by the formula `arm-none-eabi-size` users know: flash is what the
 *     image stores -- code, read-only data and initialised data -- and RAM
 *     is initialised data and .bss. A section's kind comes from its flags;
 *     its load address (where an initialiser is stored, when that is not
 *     where it runs) from the program headers;
 *   - how full each memory region is (--region, as a linker script's
 *     MEMORY block names them), counted as GNU ld and EmbLD count it: from
 *     the region's origin to the end of the last byte placed in it, by run
 *     address and, for data stored elsewhere, by load address -- so
 *     alignment padding between sections is used space, as it is;
 *   - which functions and objects are biggest (--top), with the bytes no
 *     symbol covers -- literal pools, padding, string literals -- shown
 *     per section rather than lost;
 *   - which input file or library member each byte came from (--map, a
 *     map file in GNU ld's format, which EmbLD's -Map writes);
 *   - what grew since the last build (--diff OLD), section by section and
 *     symbol by symbol;
 *   - and whether it still fits (--max-flash, --max-ram: exit 1 when not),
 *     for a build or CI script.
 *
 * ISO C and standalone, like embar and embsvd: no libelf, no binutils.
 * docs/manual/tools/embmap.md is the reference. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

/* ---- the ELF image --------------------------------------------------- */

enum kind { K_TEXT, K_RODATA, K_DATA, K_BSS };
static const char *const kind_name[] = { "text", "rodata", "data", "bss" };

struct sec {
    char *name;
    u64 addr, lma, size;
    unsigned type, flags;
    enum kind kind;
    int loaded_apart;           /* stored at lma, run at addr (lma != addr) */
    u64 covered;                /* bytes some symbol accounts for */
};

struct sym {
    char *name;
    u64 addr, size;
    int sec;                    /* index into img.s */
    int func;
};

struct img {
    const char *path;
    int is64, be;
    unsigned machine;
    struct sec *s;
    int ns;
    struct sym *y;
    int ny;
};

static void die(const char *fmt, const char *a)
{
    fprintf(stderr, "embmap: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    exit(2);
}

static unsigned char *g_buf;
static size_t g_len;
static int g_be;

static u64 rd(size_t off, int n)
{
    u64 v = 0;
    if (off + (size_t)n > g_len || off + (size_t)n < off)
        die("%s: truncated ELF file", "image");
    for (int k = 0; k < n; k++) {
        int at = g_be ? k : n - 1 - k;
        v = (v << 8) | g_buf[off + (size_t)at];
    }
    return v;
}

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        die("cannot open %s", path);
    size_t cap = 1 << 16, n = 0;
    unsigned char *p = malloc(cap);
    if (!p)
        die("out of memory reading %s", path);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            p = realloc(p, cap);
            if (!p)
                die("out of memory reading %s", path);
        }
        size_t got = fread(p + n, 1, cap - n, f);
        if (got == 0)
            break;
        n += got;
    }
    fclose(f);
    *len = n;
    return p;
}

static char *dupstr(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (!d)
        die("%s", "out of memory");
    memcpy(d, s, n);
    return d;
}

/* A NUL-terminated string at off inside the file, or "" when it runs out. */
static const char *strat(size_t off)
{
    if (off >= g_len)
        return "";
    if (!memchr(g_buf + off, 0, g_len - off))
        return "";
    return (const char *)g_buf + off;
}

#define SHT_SYMTAB  2
#define SHT_NOBITS  8
#define SHF_WRITE   1u
#define SHF_ALLOC   2u
#define SHF_EXEC    4u
#define PT_LOAD     1u
#define STT_OBJECT  1
#define STT_FUNC    2
#define EM_ARM      40

static void load(struct img *im, const char *path)
{
    memset(im, 0, sizeof *im);
    im->path = path;
    g_buf = slurp(path, &g_len);
    if (g_len < 52 || memcmp(g_buf, "\177ELF", 4) != 0)
        die("%s: not an ELF file", path);
    im->is64 = g_buf[4] == 2;
    im->be = g_buf[5] == 2;
    g_be = im->be;
    if ((g_buf[4] != 1 && g_buf[4] != 2) || (g_buf[5] != 1 && g_buf[5] != 2))
        die("%s: an ELF class or byte order this does not know", path);
    int w = im->is64 ? 8 : 4;
    im->machine = (unsigned)rd(18, 2);
    u64 phoff = rd(im->is64 ? 32 : 28, w);
    u64 shoff = rd(im->is64 ? 40 : 32, w);
    unsigned phentsize = (unsigned)rd(im->is64 ? 54 : 42, 2);
    unsigned phnum = (unsigned)rd(im->is64 ? 56 : 44, 2);
    unsigned shentsize = (unsigned)rd(im->is64 ? 58 : 46, 2);
    unsigned shnum = (unsigned)rd(im->is64 ? 60 : 48, 2);
    unsigned shstrndx = (unsigned)rd(im->is64 ? 62 : 50, 2);
    if (shoff == 0 || shnum == 0)
        die("%s: no section headers (a stripped image cannot be analysed)",
            path);

    /* the program headers' loads: where each address range is stored */
    u64 *pv = calloc(phnum ? phnum : 1, sizeof *pv);
    u64 *pp = calloc(phnum ? phnum : 1, sizeof *pp);
    u64 *pm = calloc(phnum ? phnum : 1, sizeof *pm);
    if (!pv || !pp || !pm)
        die("%s", "out of memory");
    int nload = 0;
    for (unsigned k = 0; k < phnum; k++) {
        size_t o = (size_t)phoff + (size_t)k * phentsize;
        if (rd(o, 4) != PT_LOAD)
            continue;
        if (im->is64) {
            pv[nload] = rd(o + 16, 8);
            pp[nload] = rd(o + 24, 8);
            pm[nload] = rd(o + 40, 8);
        } else {
            pv[nload] = rd(o + 8, 4);
            pp[nload] = rd(o + 12, 4);
            pm[nload] = rd(o + 20, 4);
        }
        nload++;
    }

    size_t shs = (size_t)shoff + (size_t)shstrndx * shentsize;
    u64 shstr_off = rd(shs + (im->is64 ? 24 : 16), w);
    im->s = calloc(shnum, sizeof *im->s);
    if (!im->s)
        die("%s", "out of memory");
    int symtab = -1;
    u64 *sh_off = calloc(shnum, sizeof *sh_off);
    u64 *sh_link = calloc(shnum, sizeof *sh_link);
    u64 *sh_entsize = calloc(shnum, sizeof *sh_entsize);
    int *map = malloc(shnum * sizeof *map);  /* section index -> im->s */
    if (!sh_off || !sh_link || !sh_entsize || !map)
        die("%s", "out of memory");
    for (unsigned k = 0; k < shnum; k++) {
        size_t o = (size_t)shoff + (size_t)k * shentsize;
        unsigned nameoff = (unsigned)rd(o, 4);
        unsigned type = (unsigned)rd(o + 4, 4);
        u64 flags = rd(o + 8, w);
        u64 addr = rd(o + (im->is64 ? 16 : 12), w);
        sh_off[k] = rd(o + (im->is64 ? 24 : 16), w);
        u64 size = rd(o + (im->is64 ? 32 : 20), w);
        sh_link[k] = rd(o + (im->is64 ? 40 : 24), 4);
        sh_entsize[k] = rd(o + (im->is64 ? 56 : 36), w);
        map[k] = -1;
        if (type == SHT_SYMTAB)
            symtab = (int)k;
        if (!(flags & SHF_ALLOC) || size == 0)
            continue;
        struct sec *s = &im->s[im->ns];
        map[k] = im->ns++;
        s->name = dupstr(strat((size_t)(shstr_off + nameoff)));
        s->addr = addr;
        s->size = size;
        s->type = type;
        s->flags = (unsigned)flags;
        s->lma = addr;
        for (int j = 0; j < nload; j++)
            if (addr >= pv[j] && addr < pv[j] + pm[j]) {
                s->lma = addr - pv[j] + pp[j];
                break;
            }
        if (type == SHT_NOBITS)
            s->kind = K_BSS;
        else if (flags & SHF_EXEC)
            s->kind = K_TEXT;
        else if (flags & SHF_WRITE)
            s->kind = K_DATA;
        else
            s->kind = K_RODATA;
        s->loaded_apart = s->kind != K_BSS && s->lma != s->addr;
    }

    /* the symbols: functions and objects with a size, in a section kept */
    if (symtab >= 0) {
        u64 ent = sh_entsize[symtab] ? sh_entsize[symtab]
                                     : (u64)(im->is64 ? 24 : 16);
        size_t so = (size_t)shoff + (size_t)symtab * shentsize;
        u64 size = rd(so + (im->is64 ? 32 : 20), w);
        u64 n = size / ent;
        unsigned strsec = (unsigned)sh_link[symtab];
        u64 stroff = strsec < shnum ? sh_off[strsec] : 0;
        im->y = calloc(n ? n : 1, sizeof *im->y);
        if (!im->y)
            die("%s", "out of memory");
        for (u64 k = 1; k < n; k++) {
            size_t o = (size_t)(sh_off[symtab] + k * ent);
            unsigned nameoff = (unsigned)rd(o, 4);
            unsigned info, shndx;
            u64 value, ssize;
            if (im->is64) {
                info = (unsigned)rd(o + 4, 1);
                shndx = (unsigned)rd(o + 6, 2);
                value = rd(o + 8, 8);
                ssize = rd(o + 16, 8);
            } else {
                value = rd(o + 4, 4);
                ssize = rd(o + 8, 4);
                info = (unsigned)rd(o + 12, 1);
                shndx = (unsigned)rd(o + 14, 2);
            }
            int st = (int)(info & 15);
            if ((st != STT_FUNC && st != STT_OBJECT) || ssize == 0 ||
                shndx == 0 || shndx >= shnum || map[shndx] < 0)
                continue;
            /* a Thumb function's address has the low bit set */
            if (st == STT_FUNC && im->machine == EM_ARM)
                value &= ~(u64)1;
            struct sym *y = &im->y[im->ny++];
            y->name = dupstr(strat((size_t)(stroff + nameoff)));
            y->addr = value;
            y->size = ssize;
            y->sec = map[shndx];
            y->func = st == STT_FUNC;
        }
    }
    free(pv); free(pp); free(pm);
    free(sh_off); free(sh_link); free(sh_entsize); free(map);
    free(g_buf);
    g_buf = NULL;
}

/* ---- symbols: aliases merged, coverage per section -------------------- */

static int sym_by_addr(const void *a, const void *b)
{
    const struct sym *x = a, *y = b;
    if (x->sec != y->sec)
        return x->sec < y->sec ? -1 : 1;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    if (x->size != y->size)
        return x->size > y->size ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* Two names for the same bytes (an alias, a weak and a strong) are one
 * entry: the bytes are counted once. Then each section's coverage is the
 * union of its symbols' ranges, clipped to the section. */
static void settle(struct img *im)
{
    qsort(im->y, (size_t)im->ny, sizeof *im->y, sym_by_addr);
    int out = 0;
    for (int k = 0; k < im->ny; k++) {
        if (out > 0 && im->y[out - 1].sec == im->y[k].sec &&
            im->y[out - 1].addr == im->y[k].addr &&
            im->y[out - 1].size == im->y[k].size)
            continue;
        im->y[out++] = im->y[k];
    }
    im->ny = out;
    for (int i = 0; i < im->ns; i++)
        im->s[i].covered = 0;
    u64 reach = 0;
    int cur = -1;
    for (int k = 0; k < im->ny; k++) {
        struct sym *y = &im->y[k];
        struct sec *s = &im->s[y->sec];
        if (y->sec != cur) {
            cur = y->sec;
            reach = s->addr;
        }
        u64 lo = y->addr < s->addr ? s->addr : y->addr;
        u64 hi = y->addr + y->size;
        if (hi > s->addr + s->size)
            hi = s->addr + s->size;
        if (lo < reach)
            lo = reach;
        if (hi > lo) {
            s->covered += hi - lo;
            reach = hi;
        }
    }
}

/* ---- totals ----------------------------------------------------------- */

struct totals { u64 kind[4], flash, ram; };

static void totals(const struct img *im, struct totals *t)
{
    memset(t, 0, sizeof *t);
    for (int i = 0; i < im->ns; i++) {
        const struct sec *s = &im->s[i];
        t->kind[s->kind] += s->size;
        switch (s->kind) {
        case K_TEXT: case K_RODATA:
            t->flash += s->size;
            break;
        case K_DATA:
            t->ram += s->size;
            t->flash += s->size;
            break;
        case K_BSS:
            t->ram += s->size;
            break;
        }
    }
}

/* ---- memory regions --------------------------------------------------- */

struct region { char *name; u64 origin, length, used; };
static struct region g_reg[32];
static int g_nreg;

static int parse_num(const char *p, u64 *out, const char **end)
{
    char *e;
    u64 v = strtoull(p, &e, 0);
    if (e == p)
        return 0;
    if (*e == 'K' || *e == 'k') { v <<= 10; e++; }
    else if (*e == 'M' || *e == 'm') { v <<= 20; e++; }
    else if (*e == 'G' || *e == 'g') { v <<= 30; e++; }
    *out = v;
    *end = e;
    return 1;
}

static void add_region(const char *spec)
{
    const char *c1 = strchr(spec, ':');
    const char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
    const char *e;
    struct region *r;
    if (!c1 || !c2 || g_nreg == 32)
        die("--region %s: expected NAME:ORIGIN:LENGTH (e.g. "
            "FLASH:0x08000000:512K)", spec);
    r = &g_reg[g_nreg];
    r->name = malloc((size_t)(c1 - spec) + 1);
    if (!r->name)
        die("%s", "out of memory");
    memcpy(r->name, spec, (size_t)(c1 - spec));
    r->name[c1 - spec] = 0;
    if (!parse_num(c1 + 1, &r->origin, &e) || e != c2 ||
        !parse_num(c2 + 1, &r->length, &e) || *e)
        die("--region %s: expected NAME:ORIGIN:LENGTH (e.g. "
            "FLASH:0x08000000:512K)", spec);
    g_nreg++;
}

/* A range belongs to the region its start falls in, as a linker script's
 * MEMORY assignment does -- a section run from RAM and stored in flash is
 * in both -- and the region is used up to the furthest end placed in it. */
static void place(u64 at, u64 size)
{
    for (int k = 0; k < g_nreg; k++)
        if (at >= g_reg[k].origin && at - g_reg[k].origin < g_reg[k].length) {
            u64 end = at + size - g_reg[k].origin;
            if (end > g_reg[k].used)
                g_reg[k].used = end;
            return;
        }
}

static void regions(const struct img *im)
{
    for (int k = 0; k < g_nreg; k++)
        g_reg[k].used = 0;
    for (int i = 0; i < im->ns; i++) {
        const struct sec *s = &im->s[i];
        place(s->addr, s->size);
        if (s->loaded_apart)
            place(s->lma, s->size);
    }
}

/* ---- a GNU-ld-format map: bytes by input file ------------------------- */

struct file { char *path; u64 kind[4]; };
static struct file *g_files;
static int g_nfiles, g_capfiles;

static struct file *file_for(const char *path)
{
    for (int k = 0; k < g_nfiles; k++)
        if (strcmp(g_files[k].path, path) == 0)
            return &g_files[k];
    if (g_nfiles == g_capfiles) {
        g_capfiles = g_capfiles ? g_capfiles * 2 : 64;
        g_files = realloc(g_files, (size_t)g_capfiles * sizeof *g_files);
        if (!g_files)
            die("%s", "out of memory");
    }
    struct file *f = &g_files[g_nfiles++];
    memset(f, 0, sizeof *f);
    f->path = dupstr(path);
    return f;
}

/* The section of the image an address falls in, or -1. */
static int sec_at(const struct img *im, u64 a)
{
    for (int i = 0; i < im->ns; i++)
        if (a >= im->s[i].addr && a - im->s[i].addr < im->s[i].size)
            return i;
    return -1;
}

/* Lines of the map's input-section listing:
 *
 *    .text.main     0x08000124       0x3c main.o
 *    .rodata.str1.1
 *                   0x08000400        0xc lib/libc.a(printf.o)
 *
 * a name starting with '.', an address, a size and the file -- the address
 * and the rest wrap to the next line when the name is long. Only input
 * sections that land in an allocated section of the image count, by the
 * kind of THAT section; everything else in the map (discards, the memory
 * map, symbol lines) has no file field and is skipped. */
static void read_map(const struct img *im, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[4096], pending[4096];
    int have_pending = 0;
    if (!f)
        die("cannot open %s", path);
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (p[0] == ' ' && p[1] == '.') {
            /* " .name ..." -- possibly with the rest on the next line */
            char name[1024];
            u64 a, sz;
            char file[2048];
            if (sscanf(p, " %1023s %llx %llx %2047[^\n]", name, &a, &sz,
                       file) == 4) {
                have_pending = 0;
                goto entry_have;
            }
            if (sscanf(p, " %1023s", name) == 1 && !strchr(p + 1, ' ')) {
                snprintf(pending, sizeof pending, "%s", name);
                have_pending = 1;
            } else {
                have_pending = 0;
            }
            continue;
        entry_have:
            if (sz == 0 || strncmp(file, "0x", 2) == 0)
                continue;
            {
                int si = sec_at(im, a);
                if (si >= 0)
                    file_for(file)->kind[im->s[si].kind] += sz;
            }
            continue;
        }
        if (have_pending && p[0] == ' ') {
            u64 a, sz;
            char file[2048];
            have_pending = 0;
            if (sscanf(p, " %llx %llx %2047[^\n]", &a, &sz, file) == 3 &&
                sz && strncmp(file, "0x", 2) != 0) {
                int si = sec_at(im, a);
                if (si >= 0)
                    file_for(file)->kind[im->s[si].kind] += sz;
            }
            continue;
        }
        have_pending = 0;
    }
    fclose(f);
}

/* ---- output ----------------------------------------------------------- */

static int g_json;

static void json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 0x20)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}

static void human(u64 v, char *b, size_t n)
{
    if (v >= (1ull << 20) && v % (1ull << 20) == 0)
        snprintf(b, n, "%llu MB", v >> 20);
    else if (v >= 1024 && v % 1024 == 0)
        snprintf(b, n, "%llu KB", v >> 10);
    else
        snprintf(b, n, "%llu B", v);
}

static int big_first(const void *a, const void *b)
{
    const struct sym *x = a, *y = b;
    if (x->size != y->size)
        return x->size > y->size ? -1 : 1;
    return strcmp(x->name, y->name);
}

static int file_big_first(const void *a, const void *b)
{
    const struct file *x = a, *y = b;
    u64 tx = x->kind[0] + x->kind[1] + x->kind[2] + x->kind[3];
    u64 ty = y->kind[0] + y->kind[1] + y->kind[2] + y->kind[3];
    if (tx != ty)
        return tx > ty ? -1 : 1;
    return strcmp(x->path, y->path);
}

static void report(const struct img *im, int top)
{
    struct totals t;
    totals(im, &t);
    if (g_json) {
        printf("{\n  \"image\": ");
        json_str(im->path);
        printf(",\n  \"elf\": \"%s\", \"byte_order\": \"%s\", "
               "\"machine\": %u,\n",
               im->is64 ? "ELF64" : "ELF32", im->be ? "big" : "little",
               im->machine);
        printf("  \"flash\": %llu, \"ram\": %llu,\n", t.flash, t.ram);
        printf("  \"text\": %llu, \"rodata\": %llu, \"data\": %llu, "
               "\"bss\": %llu,\n",
               t.kind[0], t.kind[1], t.kind[2], t.kind[3]);
        printf("  \"sections\": [");
        for (int i = 0; i < im->ns; i++) {
            const struct sec *s = &im->s[i];
            printf("%s\n    {\"name\": ", i ? "," : "");
            json_str(s->name);
            printf(", \"address\": %llu, \"load\": %llu, \"size\": %llu, "
                   "\"kind\": \"%s\", \"unattributed\": %llu}",
                   s->addr, s->lma, s->size, kind_name[s->kind],
                   s->size - s->covered);
        }
        printf("\n  ],\n  \"regions\": [");
        for (int k = 0; k < g_nreg; k++) {
            printf("%s\n    {\"name\": ", k ? "," : "");
            json_str(g_reg[k].name);
            printf(", \"origin\": %llu, \"length\": %llu, \"used\": %llu}",
                   g_reg[k].origin, g_reg[k].length, g_reg[k].used);
        }
        printf("\n  ]");
        return;
    }
    printf("%s: %s %s-endian, machine %u\n\n", im->path,
           im->is64 ? "ELF64" : "ELF32", im->be ? "big" : "little",
           im->machine);
    printf("%-24s %-6s %10s %18s %18s\n", "section", "kind", "size",
           "address", "load");
    for (int i = 0; i < im->ns; i++) {
        const struct sec *s = &im->s[i];
        printf("%-24s %-6s %10llu %#18llx", s->name, kind_name[s->kind],
               s->size, s->addr);
        if (s->loaded_apart)
            printf(" %#18llx", s->lma);
        printf("\n");
    }
    printf("\nflash %llu bytes, RAM %llu bytes "
           "(text %llu, rodata %llu, data %llu, bss %llu)\n",
           t.flash, t.ram, t.kind[0], t.kind[1], t.kind[2], t.kind[3]);
    if (g_nreg) {
        printf("\n%-16s %12s %12s %10s\n", "Memory region", "Used Size",
               "Region Size", "%age Used");
        for (int k = 0; k < g_nreg; k++) {
            char u[32], l[32];
            human(g_reg[k].used, u, sizeof u);
            human(g_reg[k].length, l, sizeof l);
            printf("%-16s %12s %12s %9.2f%%\n", g_reg[k].name, u, l,
                   g_reg[k].length ? 100.0 * (double)g_reg[k].used /
                                     (double)g_reg[k].length : 0.0);
        }
    }
    (void)top;
}

static void report_top(const struct img *im, int top)
{
    struct sym *v = malloc(((size_t)im->ny + 1) * sizeof *v);
    if (!v)
        die("%s", "out of memory");
    memcpy(v, im->y, (size_t)im->ny * sizeof *v);
    qsort(v, (size_t)im->ny, sizeof *v, big_first);
    int n = top < im->ny ? top : im->ny;
    if (g_json) {
        printf(",\n  \"top\": [");
        for (int k = 0; k < n; k++) {
            printf("%s\n    {\"name\": ", k ? "," : "");
            json_str(v[k].name);
            printf(", \"address\": %llu, \"size\": %llu, \"section\": ",
                   v[k].addr, v[k].size);
            json_str(im->s[v[k].sec].name);
            printf(", \"kind\": \"%s\"}", v[k].func ? "function" : "object");
        }
        printf("\n  ]");
        free(v);
        return;
    }
    printf("\n%10s  %-8s %-20s %s\n", "size", "kind", "section", "symbol");
    for (int k = 0; k < n; k++)
        printf("%10llu  %-8s %-20s %s\n", v[k].size,
               v[k].func ? "function" : "object", im->s[v[k].sec].name,
               v[k].name);
    for (int i = 0; i < im->ns; i++) {
        u64 un = im->s[i].size - im->s[i].covered;
        if (un)
            printf("%10llu  %-8s %-20s [bytes no symbol covers]\n", un, "-",
                   im->s[i].name);
    }
    free(v);
}

static void report_files(void)
{
    qsort(g_files, (size_t)g_nfiles, sizeof *g_files, file_big_first);
    if (g_json) {
        printf(",\n  \"files\": [");
        for (int k = 0; k < g_nfiles; k++) {
            const struct file *f = &g_files[k];
            printf("%s\n    {\"file\": ", k ? "," : "");
            json_str(f->path);
            printf(", \"text\": %llu, \"rodata\": %llu, \"data\": %llu, "
                   "\"bss\": %llu}",
                   f->kind[0], f->kind[1], f->kind[2], f->kind[3]);
        }
        printf("\n  ]");
        return;
    }
    printf("\n%10s %10s %10s %10s  %s\n", "text", "rodata", "data", "bss",
           "input file");
    for (int k = 0; k < g_nfiles; k++) {
        const struct file *f = &g_files[k];
        printf("%10llu %10llu %10llu %10llu  %s\n", f->kind[0], f->kind[1],
               f->kind[2], f->kind[3], f->path);
    }
}

/* ---- --diff ----------------------------------------------------------- */

struct delta { const char *name; long long old, now; };

static int delta_order(const void *a, const void *b)
{
    const struct delta *x = a, *y = b;
    long long dx = x->now - x->old, dy = y->now - y->old;
    long long ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax != ay)
        return ax > ay ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* Matched by name: a symbol's size is the sum of every symbol of that name
 * (two file-scope statics may share one), a section's likewise. */
static long long sum_sym(const struct img *im, const char *name)
{
    long long t = 0;
    for (int k = 0; k < im->ny; k++)
        if (strcmp(im->y[k].name, name) == 0)
            t += (long long)im->y[k].size;
    return t;
}

static long long sum_sec(const struct img *im, const char *name)
{
    long long t = 0;
    for (int k = 0; k < im->ns; k++)
        if (strcmp(im->s[k].name, name) == 0)
            t += (long long)im->s[k].size;
    return t;
}

static int seen_name(struct delta *d, int n, const char *name)
{
    for (int k = 0; k < n; k++)
        if (strcmp(d[k].name, name) == 0)
            return 1;
    return 0;
}

static void report_diff(const struct img *old, const struct img *now, int top)
{
    struct totals to, tn;
    totals(old, &to);
    totals(now, &tn);
    int cap = old->ny + now->ny + old->ns + now->ns + 1, n = 0, ns = 0;
    struct delta *d = malloc((size_t)cap * sizeof *d);
    struct delta *ds = malloc((size_t)cap * sizeof *ds);
    if (!d || !ds)
        die("%s", "out of memory");
    for (int pass = 0; pass < 2; pass++) {
        const struct img *im = pass ? now : old;
        for (int k = 0; k < im->ny; k++)
            if (!seen_name(d, n, im->y[k].name)) {
                d[n].name = im->y[k].name;
                d[n].old = sum_sym(old, im->y[k].name);
                d[n].now = sum_sym(now, im->y[k].name);
                n++;
            }
        for (int k = 0; k < im->ns; k++)
            if (!seen_name(ds, ns, im->s[k].name)) {
                ds[ns].name = im->s[k].name;
                ds[ns].old = sum_sec(old, im->s[k].name);
                ds[ns].now = sum_sec(now, im->s[k].name);
                ns++;
            }
    }
    qsort(d, (size_t)n, sizeof *d, delta_order);
    qsort(ds, (size_t)ns, sizeof *ds, delta_order);
    if (g_json) {
        printf(",\n  \"diff\": {\"against\": ");
        json_str(old->path);
        printf(", \"flash\": %lld, \"ram\": %lld,\n    \"sections\": [",
               (long long)tn.flash - (long long)to.flash,
               (long long)tn.ram - (long long)to.ram);
        int first = 1;
        for (int k = 0; k < ns; k++) {
            if (ds[k].old == ds[k].now)
                continue;
            printf("%s\n      {\"name\": ", first ? "" : ",");
            first = 0;
            json_str(ds[k].name);
            printf(", \"old\": %lld, \"new\": %lld}", ds[k].old, ds[k].now);
        }
        printf("\n    ],\n    \"symbols\": [");
        first = 1;
        for (int k = 0, shown = 0; k < n && shown < top; k++) {
            if (d[k].old == d[k].now)
                continue;
            printf("%s\n      {\"name\": ", first ? "" : ",");
            first = 0;
            shown++;
            json_str(d[k].name);
            printf(", \"old\": %lld, \"new\": %lld}", d[k].old, d[k].now);
        }
        printf("\n    ]}");
        free(d); free(ds);
        return;
    }
    printf("\nagainst %s: flash %+lld bytes (%llu -> %llu), RAM %+lld bytes "
           "(%llu -> %llu)\n", old->path,
           (long long)tn.flash - (long long)to.flash, to.flash, tn.flash,
           (long long)tn.ram - (long long)to.ram, to.ram, tn.ram);
    printf("\n%10s %10s %10s  %s\n", "old", "new", "delta", "section");
    for (int k = 0; k < ns; k++)
        if (ds[k].old != ds[k].now)
            printf("%10lld %10lld %+10lld  %s\n", ds[k].old, ds[k].now,
                   ds[k].now - ds[k].old, ds[k].name);
    printf("\n%10s %10s %10s  %s\n", "old", "new", "delta", "symbol");
    for (int k = 0, shown = 0; k < n && shown < top; k++) {
        if (d[k].old == d[k].now)
            continue;
        shown++;
        printf("%10lld %10lld %+10lld  %s%s\n", d[k].old, d[k].now,
               d[k].now - d[k].old, d[k].name,
               d[k].old == 0 ? " (new)" : d[k].now == 0 ? " (gone)" : "");
    }
    free(d); free(ds);
}

/* ---- main ------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "usage: embmap IMAGE [--region NAME:ORIGIN:LENGTH]... [--top N]\n"
        "                    [--map FILE.map] [--diff OLD] [--json]\n"
        "                    [--max-flash N] [--max-ram N]\n");
    exit(2);
}

/* ---- as `size` ----------------------------------------------------------
 *
 * Called by a name ending in `size` (embcc-size, arm-none-eabi-size), embmap
 * prints binutils' size table, so a build's post-link `size --format=berkeley
 * app.elf` (CMake's and every vendor Makefile's) runs unchanged. Berkeley:
 * text is every allocated section that is code or read-only, data the
 * writable ones with contents, bss the writable ones without -- binutils'
 * rule. SysV (-A): every allocated section with its size and address. Any
 * ELF, object or image, 32- or 64-bit, either byte order. */
static unsigned long long sz_rd(const unsigned char *b, size_t o, int n,
                                int be)
{
    unsigned long long v = 0;
    for (int k = 0; k < n; k++)
        v |= (unsigned long long)b[o + (size_t)(be ? n - 1 - k : k)] << (8 * k);
    return v;
}

static int size_one(const char *path, int sysv, int radix,
                    unsigned long long tot[3])
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if (!f) {
        fprintf(stderr, "size: '%s': no such file\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len + 1);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        fprintf(stderr, "size: %s: cannot read it\n", path);
        return 1;
    }
    fclose(f);
    if (len < 52 || memcmp(b, "\177ELF", 4) != 0) {
        fprintf(stderr, "size: %s: file format not recognized (an ELF object "
                        "or image is)\n", path);
        free(b);
        return 1;
    }
    int is64 = b[4] == 2, be = b[5] == 2, w = is64 ? 8 : 4;
    size_t shoff = (size_t)sz_rd(b, is64 ? 40 : 32, w, be);
    unsigned shes = (unsigned)sz_rd(b, is64 ? 58 : 46, 2, be);
    unsigned shn = (unsigned)sz_rd(b, is64 ? 60 : 48, 2, be);
    unsigned shstrndx = (unsigned)sz_rd(b, is64 ? 62 : 50, 2, be);
    if (shoff + (size_t)shn * shes > (size_t)len || shstrndx >= shn) {
        fprintf(stderr, "size: %s: the section headers run past the end\n",
                path);
        free(b);
        return 1;
    }
    size_t strs = (size_t)sz_rd(b, shoff + (size_t)shstrndx * shes +
                                       (is64 ? 24 : 16), w, be);
    unsigned long long t = 0, d = 0, z = 0, sv = 0;
    const char *fmt = radix == 8 ? "%-18s %10llo %10llo\n"
                    : radix == 16 ? "%-18s %#10llx %#10llx\n"
                    : "%-18s %10llu %10llu\n";
    if (sysv)
        printf("%s  :\n%-18s %10s %10s\n", path, "section", "size", "addr");
    for (unsigned k = 0; k < shn; k++) {
        size_t o = shoff + (size_t)k * shes;
        unsigned type = (unsigned)sz_rd(b, o + 4, 4, be);
        unsigned long long fl = sz_rd(b, o + 8, w, be);
        unsigned long long addr = sz_rd(b, o + (is64 ? 16 : 12), w, be);
        unsigned long long size = sz_rd(b, o + (is64 ? 32 : 20), w, be);
        if (type == 0)
            continue;
        if (sysv) {                           /* every section, as binutils */
            size_t no = strs + (size_t)sz_rd(b, o, 4, be);
            printf(fmt, no < (size_t)len ? (const char *)b + no : "?", size,
                   addr);
            sv += size;
        }
        if (!(fl & 2))                        /* SHF_ALLOC */
            continue;
        if ((fl & 4) || !(fl & 1))            /* exec, or not writable */
            t += size;
        else if (type != 8)                   /* SHT_NOBITS */
            d += size;
        else
            z += size;
    }
    if (sysv) {
        const char *tf = radix == 8 ? "%-18s %10llo\n\n"
                       : radix == 16 ? "%-18s %#10llx\n\n"
                       : "%-18s %10llu\n\n";
        printf(tf, "Total", sv);
    } else {
        const char *bf = radix == 8 ? "%7llo\t%7llo\t%7llo\t"
                       : radix == 16 ? "%#7llx\t%#7llx\t%#7llx\t"
                       : "%7llu\t%7llu\t%7llu\t";
        printf(bf, t, d, z);
        printf(radix == 8 ? "%7llo\t%7llx\t%s\n" : "%7llu\t%7llx\t%s\n",
               t + d + z, t + d + z, path);
    }
    tot[0] += t; tot[1] += d; tot[2] += z;
    free(b);
    return 0;
}

static int size_main(int argc, char **argv)
{
    int sysv = 0, radix = 10, totals = 0, nfiles = 0, bad = 0;
    unsigned long long tot[3] = { 0, 0, 0 };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-A") || !strcmp(a, "--format=sysv") ||
            !strcmp(a, "--format=SysV"))
            sysv = 1;
        else if (!strcmp(a, "-B") || !strcmp(a, "--format=berkeley") ||
                 !strcmp(a, "--format=Berkeley") || !strcmp(a, "-G") ||
                 !strcmp(a, "--format=gnu"))
            sysv = 0;
        else if (!strcmp(a, "-d") || !strcmp(a, "--radix=10"))
            radix = 10;
        else if (!strcmp(a, "-o") || !strcmp(a, "--radix=8"))
            radix = 8;
        else if (!strcmp(a, "-x") || !strcmp(a, "--radix=16"))
            radix = 16;
        else if (!strcmp(a, "-t") || !strcmp(a, "--totals"))
            totals = 1;
        else if (!strcmp(a, "--version")) {
            printf("size (EmbCC embmap)\n");
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "size: %s: an option this size does not take "
                            "(-A, -B, -d, -o, -x, -t)\n", a);
            return 1;
        } else
            nfiles++;
    }
    if (!sysv)
        printf("   text\t   data\t    bss\t    dec\t    hex\tfilename\n");
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-' || !argv[i][1])
            bad |= size_one(argv[i], sysv, radix, tot);
    if (!nfiles)
        bad |= size_one("a.out", sysv, radix, tot);
    if (totals && !sysv)
        printf("%7llu\t%7llu\t%7llu\t%7llu\t%7llx\t(TOTALS)\n", tot[0],
               tot[1], tot[2], tot[0] + tot[1] + tot[2],
               tot[0] + tot[1] + tot[2]);
    return bad;
}

/* ---- as `nm` --------------------------------------------------------------
 *
 * Called by a name ending in `nm`: binutils' symbol listing -- value, (with
 * -S) size, type letter, name -- sorted by name (-n by value, -p not at
 * all). The letter is binutils': U undefined, A absolute, C common, W/w
 * and V/v weak, then by section: T code, R read-only, D data, B bss,
 * lowercase for a local. -g external only, -u undefined only,
 * --defined-only, -S/--print-size, -n/-v, -p, -r. */
struct nm_sym { unsigned long long val, size; char type; const char *name; };
static int g_nm_rev;
static int nm_by_name(const void *x, const void *y)
{
    const struct nm_sym *a = x, *b = y;
    int c = strcmp(a->name, b->name);
    return g_nm_rev ? -c : c;
}
static int nm_by_val(const void *x, const void *y)
{
    const struct nm_sym *a = x, *b = y;
    int c = a->val < b->val ? -1 : a->val > b->val ? 1 : strcmp(a->name, b->name);
    return g_nm_rev ? -c : c;
}

static int nm_one(const char *path, int many, int pr_size, int ext_only,
                  int undef_only, int def_only, int sort)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;
    if (!f) {
        fprintf(stderr, "nm: '%s': no such file\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len + 1);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        fprintf(stderr, "nm: %s: cannot read it\n", path);
        return 1;
    }
    fclose(f);
    if (len < 52 || memcmp(b, "\177ELF", 4) != 0) {
        fprintf(stderr, "nm: %s: file format not recognized (an ELF object "
                        "or image is)\n", path);
        free(b);
        return 1;
    }
    int is64 = b[4] == 2, be = b[5] == 2, w = is64 ? 8 : 4;
    int arm = sz_rd(b, 18, 2, be) == 40;              /* EM_ARM */
    size_t shoff = (size_t)sz_rd(b, is64 ? 40 : 32, w, be);
    unsigned shes = (unsigned)sz_rd(b, is64 ? 58 : 46, 2, be);
    unsigned shn = (unsigned)sz_rd(b, is64 ? 60 : 48, 2, be);
    if (shoff + (size_t)shn * shes > (size_t)len) {
        fprintf(stderr, "nm: %s: the section headers run past the end\n", path);
        free(b);
        return 1;
    }
    struct nm_sym *v = NULL;
    size_t nv = 0;
    for (unsigned k = 0; k < shn; k++) {
        size_t o = shoff + (size_t)k * shes;
        if (sz_rd(b, o + 4, 4, be) != 2)              /* SHT_SYMTAB */
            continue;
        size_t off = (size_t)sz_rd(b, o + (is64 ? 24 : 16), w, be);
        size_t size = (size_t)sz_rd(b, o + (is64 ? 32 : 20), w, be);
        unsigned link = (unsigned)sz_rd(b, o + (is64 ? 40 : 24), 4, be);
        size_t ent = is64 ? 24 : 16;
        size_t so = shoff + (size_t)link * shes;
        size_t stroff = (size_t)sz_rd(b, so + (is64 ? 24 : 16), w, be);
        if (off + size > (size_t)len || link >= shn)
            continue;
        v = realloc(v, (nv + size / ent + 1) * sizeof *v);
        for (size_t e = ent; e + ent <= size; e += ent) {
            size_t q = off + e;
            unsigned name = (unsigned)sz_rd(b, q, 4, be);
            unsigned info = is64 ? b[q + 4] : b[q + 12];
            unsigned shndx = (unsigned)sz_rd(b, q + (is64 ? 6 : 14), 2, be);
            unsigned long long val = sz_rd(b, q + (is64 ? 8 : 4), w, be);
            unsigned long long sz = sz_rd(b, q + (is64 ? 16 : 8), w, be);
            unsigned bind = info >> 4, type = info & 15;
            const char *nm = (const char *)b + stroff + name;
            if (!name || type == 3 || type == 4)     /* SECTION, FILE */
                continue;
            /* a Thumb function's address, without the Thumb bit, as
             * binutils and LLVM print it */
            if (arm && type == 2)
                val &= ~1ull;
            if (nm[0] == '$' && (nm[1] == 'a' || nm[1] == 't' ||
                                 nm[1] == 'd' || nm[1] == 'x'))
                continue;                            /* ARM mapping symbols */
            char c;
            if (shndx == 0)
                c = bind == 2 ? (type == 1 ? 'v' : 'w') : 'U';
            else if (shndx == 0xfff1)
                c = 'A';
            else if (shndx == 0xfff2)
                c = 'C';
            else {
                size_t h = shoff + (size_t)shndx * shes;
                unsigned long long fl = shndx < shn ? sz_rd(b, h + 8, w, be) : 0;
                unsigned st = shndx < shn ? (unsigned)sz_rd(b, h + 4, 4, be) : 0;
                c = (fl & 4) ? 'T' : !(fl & 2) ? 'N' : !(fl & 1) ? 'R'
                  : st == 8 ? 'B' : 'D';
                if (bind == 2)
                    c = type == 1 ? 'V' : 'W';
                else if (bind == 0)
                    c = (char)(c - 'A' + 'a');
            }
            if (ext_only && bind == 0)
                continue;
            if (undef_only && shndx != 0)
                continue;
            if (def_only && shndx == 0)
                continue;
            v[nv].val = val; v[nv].size = sz; v[nv].type = c; v[nv].name = nm;
            nv++;
        }
    }
    if (sort == 1)
        qsort(v, nv, sizeof *v, nm_by_name);
    else if (sort == 2)
        qsort(v, nv, sizeof *v, nm_by_val);
    if (many)
        printf("\n%s:\n", path);
    int dw = is64 ? 16 : 8;
    for (size_t k = 0; k < nv; k++) {
        int und = v[k].type == 'U' || v[k].type == 'w' || v[k].type == 'v';
        if (und)
            printf("%*s ", dw, "");
        else
            printf("%0*llx ", dw, v[k].val);
        if (pr_size && !und)
            printf("%0*llx ", dw, v[k].size);
        printf("%c %s\n", v[k].type, v[k].name);
    }
    free(v);
    free(b);
    return 0;
}

static int nm_main(int argc, char **argv)
{
    int pr_size = 0, ext = 0, undef = 0, def = 0, sort = 1, nfiles = 0, bad = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-S") || !strcmp(a, "--print-size")) pr_size = 1;
        else if (!strcmp(a, "-g") || !strcmp(a, "--extern-only")) ext = 1;
        else if (!strcmp(a, "-u") || !strcmp(a, "--undefined-only")) undef = 1;
        else if (!strcmp(a, "--defined-only") || !strcmp(a, "-U")) def = 1;
        else if (!strcmp(a, "-n") || !strcmp(a, "-v") ||
                 !strcmp(a, "--numeric-sort")) sort = 2;
        else if (!strcmp(a, "-p") || !strcmp(a, "--no-sort")) sort = 0;
        else if (!strcmp(a, "-r") || !strcmp(a, "--reverse-sort")) g_nm_rev = 1;
        else if (!strcmp(a, "-C") || !strcmp(a, "--demangle") ||
                 !strcmp(a, "--no-demangle") || !strcmp(a, "-a") ||
                 !strcmp(a, "--debug-syms")) {}
        else if (!strcmp(a, "--version")) {
            printf("nm (EmbCC embmap)\n");
            return 0;
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "nm: %s: an option this nm does not take (-S, -g, "
                            "-u, --defined-only, -n, -p, -r)\n", a);
            return 1;
        } else
            nfiles++;
    }
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-' || !argv[i][1])
            bad |= nm_one(argv[i], nfiles > 1, pr_size, ext, undef, def, sort);
    if (!nfiles)
        bad |= nm_one("a.out", 0, pr_size, ext, undef, def, sort);
    return bad;
}

int main(int argc, char **argv)
{
    size_t n0 = strlen(argv[0]);
    if (n0 >= 4 && !strcmp(argv[0] + n0 - 4, "size"))
        return size_main(argc, argv);
    if (n0 >= 2 && !strcmp(argv[0] + n0 - 2, "nm") &&
        (n0 == 2 || argv[0][n0 - 3] == '-' || argv[0][n0 - 3] == '/'))
        return nm_main(argc, argv);
    const char *image = NULL, *mapfile = NULL, *diff = NULL;
    int top = 0, want_top = 0;
    u64 max_flash = 0, max_ram = 0;
    int have_mf = 0, have_mr = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *e;
        if (strcmp(a, "--region") == 0 && i + 1 < argc) {
            add_region(argv[++i]);
        } else if (strcmp(a, "--top") == 0 && i + 1 < argc) {
            top = atoi(argv[++i]);
            want_top = 1;
            if (top <= 0)
                die("--top %s: expected a positive count", argv[i]);
        } else if (strcmp(a, "--map") == 0 && i + 1 < argc) {
            mapfile = argv[++i];
        } else if (strcmp(a, "--diff") == 0 && i + 1 < argc) {
            diff = argv[++i];
        } else if (strcmp(a, "--json") == 0) {
            g_json = 1;
        } else if (strcmp(a, "--max-flash") == 0 && i + 1 < argc) {
            if (!parse_num(argv[++i], &max_flash, &e) || *e)
                die("--max-flash %s: expected a size (e.g. 256K)", argv[i]);
            have_mf = 1;
        } else if (strcmp(a, "--max-ram") == 0 && i + 1 < argc) {
            if (!parse_num(argv[++i], &max_ram, &e) || *e)
                die("--max-ram %s: expected a size (e.g. 64K)", argv[i]);
            have_mr = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
        } else if (a[0] == '-') {
            die("unknown option %s (embmap --help)", a);
        } else if (!image) {
            image = a;
        } else {
            die("one image at a time (%s); compare two with --diff", a);
        }
    }
    if (!image)
        usage();

    struct img im, old;
    load(&im, image);
    settle(&im);
    regions(&im);
    report(&im, top);
    if (want_top || (!diff && !mapfile && !g_json))
        report_top(&im, want_top ? top : 10);
    if (mapfile) {
        read_map(&im, mapfile);
        report_files();
    }
    if (diff) {
        load(&old, diff);
        settle(&old);
        report_diff(&old, &im, want_top ? top : 20);
    }
    if (g_json)
        printf("\n}\n");

    struct totals t;
    totals(&im, &t);
    int over = 0;
    if (have_mf && t.flash > max_flash) {
        fprintf(stderr, "embmap: %s needs %llu bytes of flash and the "
                "budget is %llu (--max-flash)\n", image, t.flash, max_flash);
        over = 1;
    }
    if (have_mr && t.ram > max_ram) {
        fprintf(stderr, "embmap: %s needs %llu bytes of RAM and the budget "
                "is %llu (--max-ram)\n", image, t.ram, max_ram);
        over = 1;
    }
    for (int k = 0; k < g_nreg; k++)
        if (g_reg[k].used > g_reg[k].length) {
            fprintf(stderr, "embmap: region %s overflows: %llu bytes used "
                    "of %llu\n", g_reg[k].name, g_reg[k].used,
                    g_reg[k].length);
            over = 1;
        }
    return over;
}
