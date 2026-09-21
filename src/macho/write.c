#include "write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../platform/platform.h"

#define MAX_SECTIONS 32

struct buf {
    unsigned char *p;
    size_t len, cap;
};

static void buf_append(struct buf *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n)
            cap *= 2;
        b->p = realloc(b->p, cap);
        if (!b->p) {
            fprintf(stderr, "embcc: out of memory\n");
            fatal_unwind();
        }
        b->cap = cap;
    }
    if (data)
        memcpy(b->p + b->len, data, n);
    else
        memset(b->p + b->len, 0, n);
    b->len += n;
}

struct sect {
    char segname[16];
    char sectname[16];
    unsigned int flags;
    unsigned int align;            /* exponent */
    unsigned long long size;
    struct buf data;               /* empty for a zero-filled section */
    int zerofill;
    struct buf relocs;             /* struct relocation_info, packed */
    unsigned int nreloc;
    unsigned long long addr;       /* assigned at write time */
    unsigned int offset;           /* ... and the file offset */
};

struct machow {
    int cputype, cpusubtype;
    struct sect sect[MAX_SECTIONS];
    int nsect;
    struct buf syms;               /* struct nlist_64, packed */
    int nsyms;
    struct buf strtab;
};

struct machow *machow_new(int cputype, int cpusubtype)
{
    struct machow *w = xcalloc(1, sizeof *w);
    w->cputype = cputype;
    w->cpusubtype = cpusubtype;
    /* Index 0 of a string table is the empty name, so the table starts
     * with the NUL that terminates it. */
    buf_append(&w->strtab, "", 1);
    return w;
}

void machow_free(struct machow *w)
{
    if (!w)
        return;
    for (int i = 0; i < w->nsect; i++) {
        free(w->sect[i].data.p);
        free(w->sect[i].relocs.p);
    }
    free(w->syms.p);
    free(w->strtab.p);
    free(w);
}

static unsigned long long round_up(unsigned long long v, unsigned long long a)
{
    return a ? (v + a - 1) / a * a : v;
}

/* The 16-byte name fields are NOT NUL-terminated when the name is
 * exactly sixteen characters; they are padded with zeros otherwise.
 * strncpy is the one call whose surprising behaviour is the required
 * one here, which is worth saying because it looks like a bug. */
static void set_name16(char *dst, const char *src)
{
    memset(dst, 0, 16);
    strncpy(dst, src, 16);
}

int machow_add_section(struct machow *w, const char *segname,
                       const char *sectname, unsigned int flags,
                       const void *data, unsigned long long size,
                       unsigned int align)
{
    if (w->nsect == MAX_SECTIONS) {
        fprintf(stderr, "embcc: too many sections for one Mach-O object\n");
        fatal_unwind();
    }
    struct sect *s = &w->sect[w->nsect];
    set_name16(s->segname, segname);
    set_name16(s->sectname, sectname);
    s->flags = flags;
    s->align = align;
    s->size = size;
    /* The address is fixed NOW rather than at write time, because a
     * symbol added next needs it: a Mach-O symbol's n_value is an
     * address in the object's own space, so a function at offset 0 of
     * __text and a global at offset 0 of __data do not both have
     * value 0. ld says so plainly -- "symbol is ignored, because its
     * address isn't in its designated section". */
    unsigned long long a = 1ULL << align;
    s->addr = w->nsect ? round_up(w->sect[w->nsect - 1].addr +
                                  w->sect[w->nsect - 1].size, a) : 0;
    s->zerofill = (flags & 0xff) == S_ZEROFILL;
    if (!s->zerofill && size)
        buf_append(&s->data, data, (size_t)size);
    return ++w->nsect;              /* 1-based, as n_sect wants */
}

static int add_sym2(struct machow *w, const char *name, int prefix,
                    unsigned long long value, int sect, int ext, int weak)
{
    struct nlist_64 n;
    memset(&n, 0, sizeof n);
    n.n_strx = (uint32_t)w->strtab.len;
    if (name && *name) {
        if (prefix)
            buf_append(&w->strtab, "_", 1);
        buf_append(&w->strtab, name, strlen(name) + 1);
    } else {
        buf_append(&w->strtab, "", 1);
    }
    n.n_type = (uint8_t)((sect ? N_SECT : N_UNDF) | (ext ? N_EXT : 0));
    n.n_sect = (uint8_t)sect;
    /* Defined and undefined weakness are different bits: a definition
     * that may be replaced, against a reference that may go unmet. */
    if (weak)
        n.n_desc = (uint16_t)(sect ? N_WEAK_DEF : N_WEAK_REF);
    n.n_value = sect ? w->sect[sect - 1].addr + value : 0;
    buf_append(&w->syms, &n, sizeof n);
    return ++w->nsyms;              /* 1-based: see write.h */
}

/* The platform's leading underscore is applied here, so that nothing
 * above this file has to know the target's symbol convention. */
int machow_add_symbol(struct machow *w, const char *name,
                      unsigned long long value, int sect, int ext)
{
    return add_sym2(w, name, 1, value, sect, ext, 0);
}

int machow_add_symbol_weak(struct machow *w, const char *name,
                           unsigned long long value, int sect, int ext)
{
    return add_sym2(w, name, 1, value, sect, ext, 1);
}

int machow_add_symbol_raw(struct machow *w, const char *name,
                          unsigned long long value, int sect, int ext)
{
    return add_sym2(w, name, 0, value, sect, ext, 0);
}

void machow_add_reloc(struct machow *w, int sect, unsigned long long offset,
                      int sym, int type, int pcrel, int length, long addend)
{
    if (sect < 1 || sect > w->nsect || sym < 1)
        return;
    sym--;                          /* the handle is 1-based; the file is not */
    struct sect *s = &w->sect[sect - 1];
    struct relocation_info r;

    /* An addend has nowhere to live in a Mach-O relocation, so each
     * target carries it the way its own tools do, and aarch64 uses two
     * different ways depending on WHAT is being patched.
     *
     * An instruction field -- a 21-bit page, a 12-bit offset, a 26-bit
     * branch -- has no room for it, so an ARM64_RELOC_ADDEND entry goes
     * FIRST, carrying the addend in the place a symbol number would
     * otherwise sit; the pair is read together.
     *
     * A DATA word has room, so the addend belongs in the word and
     * ARM64_RELOC_ADDEND is rejected outright: ld says "relocation is
     * not supported: r_type=10 ... r_length=3". The caller stores it,
     * which is why the driver patches the data buffer before handing
     * the section over. */
    int instr_field = type == ARM64_RELOC_PAGE21 ||
                      type == ARM64_RELOC_PAGEOFF12 ||
                      type == ARM64_RELOC_BRANCH26 ||
                      type == ARM64_RELOC_GOT_LOAD_PAGE21 ||
                      type == ARM64_RELOC_GOT_LOAD_PAGEOFF12;
    if (addend && w->cputype == CPU_TYPE_ARM64 && instr_field) {
        r.r_address = (int32_t)offset;
        r.r_packed = macho_reloc_pack((uint32_t)addend, 0, length, 0,
                                      ARM64_RELOC_ADDEND);
        buf_append(&s->relocs, &r, sizeof r);
        s->nreloc++;
    }
    r.r_address = (int32_t)offset;
    r.r_packed = macho_reloc_pack((uint32_t)sym, pcrel, length, 1, type);
    buf_append(&s->relocs, &r, sizeof r);
    s->nreloc++;
}

int machow_write(struct machow *w, const char *path)
{
    /* ---- layout -------------------------------------------------------
     *
     * header, load commands, section data, relocations, symbols, strings.
     *
     * In an MH_OBJECT every section starts at address 0 and they are laid
     * out consecutively; the linker assigns real addresses. The file
     * offset and the address advance together, which is what lets a
     * relocation's r_address be an offset from its own section. */
    uint32_t ncmds = 3;             /* SEGMENT_64, BUILD_VERSION, SYMTAB */
    uint32_t sizeofcmds =
        (uint32_t)(sizeof(struct segment_command_64) +
                   (size_t)w->nsect * sizeof(struct section_64) +
                   sizeof(struct build_version_command) +
                   sizeof(struct symtab_command));

    unsigned long long off = sizeof(struct mach_header_64) + sizeofcmds;
    unsigned long long addr = 0;
    for (int i = 0; i < w->nsect; i++) {
        struct sect *s = &w->sect[i];
        unsigned long long a = 1ULL << s->align;
        /* s->addr was fixed when the section was added, so that symbols
         * could be given real addresses; this only tracks the end. */
        addr = s->addr + s->size;
        if (s->zerofill) {
            s->offset = 0;          /* nothing in the file */
            continue;
        }
        off = round_up(off, a);
        s->offset = (uint32_t)off;
        off += s->size;
    }
    /* Relocations after all section data, symbols after those. */
    unsigned long long reloff = round_up(off, 4);
    unsigned long long p = reloff;
    for (int i = 0; i < w->nsect; i++) {
        w->sect[i].relocs.len = w->sect[i].relocs.len;   /* (already packed) */
        p += w->sect[i].relocs.len;
    }
    unsigned long long symoff = round_up(p, 8);
    unsigned long long stroff = symoff + w->syms.len;
    unsigned long long total = stroff + w->strtab.len;

    struct buf out = { NULL, 0, 0 };
    struct mach_header_64 mh;
    memset(&mh, 0, sizeof mh);
    mh.magic = MH_MAGIC_64;
    mh.cputype = w->cputype;
    mh.cpusubtype = w->cpusubtype;
    mh.filetype = MH_OBJECT;
    mh.ncmds = ncmds;
    mh.sizeofcmds = sizeofcmds;
    mh.flags = MH_SUBSECTIONS_VIA_SYMBOLS;
    buf_append(&out, &mh, sizeof mh);

    struct segment_command_64 sg;
    memset(&sg, 0, sizeof sg);
    sg.cmd = LC_SEGMENT_64;
    sg.cmdsize = (uint32_t)(sizeof sg +
                            (size_t)w->nsect * sizeof(struct section_64));
    /* An object's one segment has the EMPTY name: it is not __TEXT or
     * __DATA, it is the anonymous container those sections are labelled
     * for. */
    memset(sg.segname, 0, sizeof sg.segname);
    sg.vmaddr = 0;
    sg.vmsize = addr;
    sg.fileoff = w->nsect ? w->sect[0].offset : 0;
    sg.filesize = w->nsect ? off - sg.fileoff : 0;
    sg.maxprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    sg.initprot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    sg.nsects = (uint32_t)w->nsect;
    sg.flags = 0;
    buf_append(&out, &sg, sizeof sg);

    unsigned long long rp = reloff;
    for (int i = 0; i < w->nsect; i++) {
        struct sect *s = &w->sect[i];
        struct section_64 sc;
        memset(&sc, 0, sizeof sc);
        memcpy(sc.sectname, s->sectname, 16);
        memcpy(sc.segname, s->segname, 16);
        sc.addr = s->addr;
        sc.size = s->size;
        sc.offset = s->offset;
        sc.align = s->align;
        sc.reloff = s->nreloc ? (uint32_t)rp : 0;
        sc.nreloc = s->nreloc;
        sc.flags = s->flags;
        rp += s->relocs.len;
        buf_append(&out, &sc, sizeof sc);
    }

    /* Without a platform recorded, ld refuses the object outright. */
    struct build_version_command bv;
    memset(&bv, 0, sizeof bv);
    bv.cmd = LC_BUILD_VERSION;
    bv.cmdsize = (uint32_t)sizeof bv;
    bv.platform = PLATFORM_MACOS;
    bv.minos = (11u << 16);          /* 11.0.0 */
    bv.sdk = 0;
    bv.ntools = 0;
    buf_append(&out, &bv, sizeof bv);

    struct symtab_command st;
    memset(&st, 0, sizeof st);
    st.cmd = LC_SYMTAB;
    st.cmdsize = (uint32_t)sizeof st;
    st.symoff = (uint32_t)symoff;
    st.nsyms = (uint32_t)w->nsyms;
    st.stroff = (uint32_t)stroff;
    st.strsize = (uint32_t)w->strtab.len;
    buf_append(&out, &st, sizeof st);

    for (int i = 0; i < w->nsect; i++) {
        struct sect *s = &w->sect[i];
        if (s->zerofill || !s->size)
            continue;
        while (out.len < s->offset)
            buf_append(&out, NULL, 1);
        buf_append(&out, s->data.p, (size_t)s->size);
    }
    while (out.len < reloff)
        buf_append(&out, NULL, 1);
    for (int i = 0; i < w->nsect; i++)
        if (w->sect[i].relocs.len)
            buf_append(&out, w->sect[i].relocs.p, w->sect[i].relocs.len);
    while (out.len < symoff)
        buf_append(&out, NULL, 1);
    buf_append(&out, w->syms.p, w->syms.len);
    buf_append(&out, w->strtab.p, w->strtab.len);

    if (out.len != total) {
        fprintf(stderr, "embcc: internal error: Mach-O layout %zu, "
                        "expected %llu\n", out.len, total);
        free(out.p);
        return -1;
    }
    /* 0 on success, -1 on failure -- the same convention elf/write.c
     * uses, and the opposite of the truthiness this first read it as. */
    int rc = plat_write_file(path, out.p, out.len);
    if (rc != 0)
        fprintf(stderr, "embcc: cannot write %s\n", path);
    free(out.p);
    return rc;
}
