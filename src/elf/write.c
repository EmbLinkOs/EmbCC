#include "write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../platform/platform.h"
#include "../arch/target.h"

/* Growable byte buffer, used for section payloads and string tables. */
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
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

/* Appends s (with its NUL) and returns its offset — the strtab primitive. */
static Elf64_Word strtab_add(struct buf *b, const char *s)
{
    Elf64_Word off = (Elf64_Word)b->len;
    buf_append(b, s, strlen(s) + 1);
    return off;
}

struct section {
    Elf64_Shdr hdr;
    struct buf data;
};

/* Sections and relocation groups grow as needed: -ffunction-sections
 * makes a section and a relocation group per function, where a fixed 32
 * and 6 held one .text and one .data. The limit that remains is ELF's
 * own: a section index at or above SHN_LORESERVE needs the extended
 * numbering (SHN_XINDEX), which this writer does not produce. */

/* Relocations are grouped by the section they apply to; each group emits
 * its own .rela.<name> section. One group (.text) is the common case; a
 * global initializer pointing into .rodata adds a .data group. */
struct rela_group {
    int target;          /* section index the relocations apply to */
    struct buf rela;     /* array of Elf64_Rela */
    int nrela;
    int sec_ndx;         /* the .rela.<name> section, set at write time */
};

struct elfw {
    struct section *sec;
    int nsec, capsec;
    int *grp_of;         /* per section: its relocation group + 1, or 0 */
    struct buf symtab;   /* array of Elf64_Sym */
    int nsym;
    int nlocal;          /* symbols [0, nlocal) are STB_LOCAL */
    int globals_started; /* set once a non-local is added */
    struct buf strtab;   /* symbol names */
    struct buf shstrtab; /* section names */
    struct rela_group *relagrp;
    int nrelagrp, caprelagrp;
    int machine;         /* e_machine, fixed at elfw_new */
    /* ELFCLASS32 rather than 64. Decided here once and never passed in.
     * It used to be read off the MACHINE alone -- ARMv7-M is 32-bit and
     * everything else was not -- and RISC-V broke that: EM_RISCV is both
     * widths under one number, so the class comes from the target's
     * pointer size and the machine only says which target. */
    int elf32;
    Elf64_Word eflags;   /* e_flags: the EABI version on ARM, 0 elsewhere */
    /* REL rather than RELA: o32 MIPS keeps each addend in the field it
     * relocates (target_elf_uses_rel), so the writer stores them there
     * and emits .rel.<name> sections of Elf32_Rel. */
    int rel;
};

struct elfw *elfw_new(int machine)
{
    struct elfw *w = calloc(1, sizeof *w);
    if (!w) {
        fprintf(stderr, "embcc: out of memory\n");
        fatal_unwind();
    }
    w->machine = machine;
    /* <= 4, not == 4: AVR's pointer is TWO bytes and its objects are
     * ELFCLASS32 like every other small machine's. Asking for equality
     * gave it an ELF64 header, which readelf accepted and every AVR tool
     * rejected. */
    w->elf32 = target_ptr_size() <= 4;
    w->rel = target_elf_uses_rel(target_get());
    if (machine == EM_ARM)
        w->eflags = EF_ARM_EABI_VER5;
    /* RISC-V's and AVR's come from the target (elfw_set_flags): whether
     * the C extension is on, and which AVR architecture. */
    /* Index 0 is reserved in every table it manages. */
    w->capsec = 16;
    w->sec = xcalloc((size_t)w->capsec, sizeof *w->sec);
    w->grp_of = xcalloc((size_t)w->capsec, sizeof *w->grp_of);
    w->nsec = 1; /* SHT_NULL section */
    strtab_add(&w->strtab, "");
    strtab_add(&w->shstrtab, "");
    Elf64_Sym null_sym;
    memset(&null_sym, 0, sizeof null_sym);
    buf_append(&w->symtab, &null_sym, sizeof null_sym);
    w->nsym = 1;
    w->nlocal = 1;
    return w;
}

void elfw_set_flags(struct elfw *w, unsigned long flags)
{
    w->eflags = (Elf64_Word)flags;
}

void elfw_free(struct elfw *w)
{
    if (!w)
        return;
    for (int i = 0; i < w->nsec; i++)
        free(w->sec[i].data.p);
    free(w->symtab.p);
    free(w->strtab.p);
    free(w->shstrtab.p);
    for (int i = 0; i < w->nrelagrp; i++)
        free(w->relagrp[i].rela.p);
    free(w->relagrp);
    free(w->sec);
    free(w->grp_of);
    free(w);
}

/* A new section slot, zeroed; its index is w->nsec before the call. */
static struct section *sec_slot(struct elfw *w)
{
    if (w->nsec >= SHN_LORESERVE) {
        fprintf(stderr, "embcc: elf writer: more than %d sections, which "
                "needs ELF's extended section numbering\n", SHN_LORESERVE);
        fatal_unwind();
    }
    if (w->nsec == w->capsec) {
        w->capsec *= 2;
        w->sec = xrealloc(w->sec, (size_t)w->capsec * sizeof *w->sec);
        w->grp_of = xrealloc(w->grp_of, (size_t)w->capsec * sizeof *w->grp_of);
        memset(w->grp_of + w->nsec, 0,
               (size_t)(w->capsec - w->nsec) * sizeof *w->grp_of);
    }
    struct section *s = &w->sec[w->nsec];
    memset(s, 0, sizeof *s);
    return s;
}

int elfw_add_section(struct elfw *w, const char *name, Elf64_Word type,
                     Elf64_Xword flags, const void *data, Elf64_Xword size,
                     Elf64_Xword addralign)
{
    struct section *s = sec_slot(w);
    memset(s, 0, sizeof *s);
    s->hdr.sh_name = strtab_add(&w->shstrtab, name);
    s->hdr.sh_type = type;
    s->hdr.sh_flags = flags;
    s->hdr.sh_size = size;
    s->hdr.sh_addralign = addralign;
    if (size && type != SHT_NOBITS)
        buf_append(&s->data, data, (size_t)size);
    return w->nsec++;
}

void elfw_symbol_visibility(struct elfw *w, int sym, int stv)
{
    /* The table is a byte buffer of Elf64_Sym, so the entry is reached
     * by index rather than held as a pointer -- which is also why this
     * takes the index elfw_add_symbol returned. */
    if (sym <= 0 || sym >= w->nsym)
        return;
    Elf64_Sym *tab = (Elf64_Sym *)w->symtab.p;
    tab[sym].st_other = (Elf64_Uchar)(stv & 3);
}

int elfw_add_symbol(struct elfw *w, const char *name, Elf64_Addr value,
                    Elf64_Xword size, Elf64_Uchar info, Elf64_Half shndx)
{
    int is_local = (info >> 4) == STB_LOCAL;
    if (is_local && w->globals_started) {
        /* The gABI requires locals before globals (sh_info is the split
         * point). Refuse loudly instead of reordering behind the caller. */
        fprintf(stderr,
                "embcc: elf writer: local symbol '%s' added after globals\n",
                name);
        fatal_unwind();
    }
    if (!is_local)
        w->globals_started = 1;
    else
        w->nlocal++;

    Elf64_Sym sym;
    memset(&sym, 0, sizeof sym);
    sym.st_name = name && *name ? strtab_add(&w->strtab, name) : 0;
    sym.st_info = info;
    sym.st_shndx = shndx;
    sym.st_value = value;
    sym.st_size = size;
    buf_append(&w->symtab, &sym, sizeof sym);
    return w->nsym++;
}

static Elf64_Off align_up(Elf64_Off off, Elf64_Xword align)
{
    if (align < 2)
        return off;
    return (off + align - 1) & ~(align - 1);
}

void elfw_add_rela(struct elfw *w, int target_ndx, Elf64_Addr offset,
                   int sym, int type, long addend)
{
    /* Route to the group for this target section, creating it on first
     * use. Grouping keeps each .rela.<name> pointing at exactly one
     * section, as the gABI requires. */
    struct rela_group *gp = NULL;
    if (target_ndx <= 0 || target_ndx >= w->nsec) {
        fprintf(stderr, "embcc: elf writer: a relocation for section %d, "
                "which does not exist\n", target_ndx);
        fatal_unwind();
    }
    if (w->grp_of[target_ndx]) {
        gp = &w->relagrp[w->grp_of[target_ndx] - 1];
    } else {
        if (w->nrelagrp == w->caprelagrp) {
            w->caprelagrp = w->caprelagrp ? 2 * w->caprelagrp : 8;
            w->relagrp = xrealloc(w->relagrp, (size_t)w->caprelagrp *
                                              sizeof *w->relagrp);
        }
        gp = &w->relagrp[w->nrelagrp++];
        memset(gp, 0, sizeof *gp);
        gp->target = target_ndx;
        w->grp_of[target_ndx] = w->nrelagrp;
    }
    Elf64_Rela r;
    r.r_offset = offset;
    r.r_info = ELF64_R_INFO((Elf64_Xword)sym, (Elf64_Xword)type);
    r.r_addend = addend;
    buf_append(&gp->rela, &r, sizeof r);
    gp->nrela++;
}

int elfw_write(struct elfw *w, const char *path)
{
    /* Materialize the bookkeeping sections after the user's: one
     * .rela.<target> per relocation group (sh_link/sh_info patched below
     * once the symtab index exists), then .symtab/.strtab/.shstrtab. The
     * name is ".rela" + the target's own name (".rela.text", ".rela.data"). */
    for (int i = 0; i < w->nrelagrp; i++) {
        struct rela_group *gp = &w->relagrp[i];
        const char *tname = (const char *)w->shstrtab.p +
                            w->sec[gp->target].hdr.sh_name;
        /* copied before the add, which may move the string table */
        size_t tl = strlen(tname);
        size_t pl = w->rel ? 4 : 5;
        char *rname = xmalloc(tl + 6);
        memcpy(rname, w->rel ? ".rel" : ".rela", pl);
        memcpy(rname + pl, tname, tl + 1);
        gp->sec_ndx = elfw_add_section(w, rname, w->rel ? SHT_REL : SHT_RELA,
                                       SHF_INFO_LINK, gp->rela.p,
                                       gp->rela.len, w->rel ? 4 : 8);
        free(rname);
    }
    int symtab_ndx = elfw_add_section(w, ".symtab", SHT_SYMTAB, 0,
                                      w->symtab.p, w->symtab.len, 8);
    int strtab_ndx = elfw_add_section(w, ".strtab", SHT_STRTAB, 0,
                                      w->strtab.p, w->strtab.len, 1);
    /* .shstrtab names itself, so its payload is copied only after
     * strtab_add has run for it. */
    int shstr_ndx = w->nsec;
    Elf64_Word shstr_name = strtab_add(&w->shstrtab, ".shstrtab");
    struct section *shstr = sec_slot(w);
    w->nsec++;
    shstr->hdr.sh_name = shstr_name;
    shstr->hdr.sh_type = SHT_STRTAB;
    shstr->hdr.sh_size = w->shstrtab.len;
    shstr->hdr.sh_addralign = 1;
    buf_append(&shstr->data, w->shstrtab.p, w->shstrtab.len);

    w->sec[symtab_ndx].hdr.sh_link = (Elf64_Word)strtab_ndx;
    w->sec[symtab_ndx].hdr.sh_info = (Elf64_Word)w->nlocal;
    w->sec[symtab_ndx].hdr.sh_entsize =
        w->elf32 ? sizeof(Elf32_Sym) : sizeof(Elf64_Sym);
    for (int i = 0; i < w->nrelagrp; i++) {
        struct rela_group *gp = &w->relagrp[i];
        w->sec[gp->sec_ndx].hdr.sh_link = (Elf64_Word)symtab_ndx;
        w->sec[gp->sec_ndx].hdr.sh_info = (Elf64_Word)gp->target;
        w->sec[gp->sec_ndx].hdr.sh_entsize =
            w->rel ? sizeof(Elf32_Rel)
                   : w->elf32 ? sizeof(Elf32_Rela) : sizeof(Elf64_Rela);
    }

    /* The symbol table and the relocation tables were built as arrays of
     * the 64-bit structures; on a 32-bit target they are rewritten in
     * place as the narrower ones before anything is laid out, because
     * every offset below is computed from the sizes they end up. Field
     * by field, since Elf32_Sym is not Elf64_Sym with shorter members --
     * it puts st_value and st_size before st_info rather than after. */
    if (w->elf32) {
        struct section *sy = &w->sec[symtab_ndx];
        Elf64_Sym *in = (Elf64_Sym *)sy->data.p;
        int n = (int)(sy->data.len / sizeof(Elf64_Sym));
        Elf32_Sym *out = xmalloc((size_t)(n ? n : 1) * sizeof *out);
        for (int k = 0; k < n; k++) {
            out[k].st_name  = in[k].st_name;
            out[k].st_value = (Elf32_Addr)in[k].st_value;
            out[k].st_size  = (Elf32_Word)in[k].st_size;
            out[k].st_info  = in[k].st_info;
            out[k].st_other = in[k].st_other;
            out[k].st_shndx = in[k].st_shndx;
        }
        free(sy->data.p);
        sy->data.p = (unsigned char *)out;
        sy->data.len = sy->data.cap = (size_t)n * sizeof *out;
        sy->hdr.sh_size = sy->data.len;

        for (int i = 0; w->rel && i < w->nrelagrp; i++) {
            /* REL: each addend goes into the field it relocates, in the
             * shape that relocation type's linker reads it back, and the
             * entry keeps only the offset and the info word. */
            struct section *rs = &w->sec[w->relagrp[i].sec_ndx];
            struct section *ts = &w->sec[w->relagrp[i].target];
            Elf64_Rela *ri = (Elf64_Rela *)rs->data.p;
            int m = (int)(rs->data.len / sizeof(Elf64_Rela));
            Elf32_Rel *ro = xmalloc((size_t)(m ? m : 1) * sizeof *ro);
            for (int k = 0; k < m; k++) {
                int type = (int)ELF64_R_TYPE(ri[k].r_info);
                if (ts->hdr.sh_type == SHT_NOBITS ||
                    ri[k].r_offset + 4 > ts->data.len ||
                    !target_rel_put_addend(target_get(), type,
                                           ts->data.p + ri[k].r_offset,
                                           (long)ri[k].r_addend)) {
                    fprintf(stderr, "embcc: elf writer: cannot store the "
                            "addend of a type %d relocation at offset %lu\n",
                            type, (unsigned long)ri[k].r_offset);
                    fatal_unwind();
                }
                ro[k].r_offset = (Elf32_Addr)ri[k].r_offset;
                ro[k].r_info = ELF32_R_INFO(ELF64_R_SYM(ri[k].r_info),
                                            (unsigned)type);
            }
            free(rs->data.p);
            rs->data.p = (unsigned char *)ro;
            rs->data.len = rs->data.cap = (size_t)m * sizeof *ro;
            rs->hdr.sh_size = rs->data.len;
        }
        for (int i = 0; !w->rel && i < w->nrelagrp; i++) {
            struct section *rs = &w->sec[w->relagrp[i].sec_ndx];
            Elf64_Rela *ri = (Elf64_Rela *)rs->data.p;
            int m = (int)(rs->data.len / sizeof(Elf64_Rela));
            Elf32_Rela *ro = xmalloc((size_t)(m ? m : 1) * sizeof *ro);
            for (int k = 0; k < m; k++) {
                ro[k].r_offset = (Elf32_Addr)ri[k].r_offset;
                ro[k].r_info = ELF32_R_INFO(ELF64_R_SYM(ri[k].r_info),
                                            ELF64_R_TYPE(ri[k].r_info));
                ro[k].r_addend = (int)ri[k].r_addend;
            }
            free(rs->data.p);
            rs->data.p = (unsigned char *)ro;
            rs->data.len = rs->data.cap = (size_t)m * sizeof *ro;
            rs->hdr.sh_size = rs->data.len;
        }
    }

    /* Lay out: ehdr, section payloads, then the section header table. */
    Elf64_Off off = w->elf32 ? sizeof(Elf32_Ehdr) : sizeof(Elf64_Ehdr);
    for (int i = 1; i < w->nsec; i++) {
        struct section *s = &w->sec[i];
        off = align_up(off, s->hdr.sh_addralign);
        s->hdr.sh_offset = off;
        if (s->hdr.sh_type != SHT_NOBITS)
            off += s->hdr.sh_size;
    }
    Elf64_Off shoff = align_up(off, w->elf32 ? 4 : 8);

    Elf64_Word shentsize = w->elf32 ? (Elf64_Word)sizeof(Elf32_Shdr)
                                    : (Elf64_Word)sizeof(Elf64_Shdr);
    Elf64_Ehdr eh;
    memset(&eh, 0, sizeof eh);
    eh.e_ident[EI_MAG0] = ELFMAG0;
    eh.e_ident[EI_MAG1] = ELFMAG1;
    eh.e_ident[EI_MAG2] = ELFMAG2;
    eh.e_ident[EI_MAG3] = ELFMAG3;
    eh.e_ident[EI_CLASS] = w->elf32 ? ELFCLASS32 : ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_REL;
    eh.e_machine = (Elf64_Half)w->machine;
    eh.e_version = EV_CURRENT;
    eh.e_shoff = shoff;
    eh.e_flags = w->eflags;
    eh.e_ehsize = (Elf64_Half)(w->elf32 ? sizeof(Elf32_Ehdr)
                                        : sizeof(Elf64_Ehdr));
    eh.e_shentsize = (Elf64_Half)shentsize;
    eh.e_shnum = (Elf64_Half)w->nsec;
    eh.e_shstrndx = (Elf64_Half)shstr_ndx;

    /* The image is assembled whole and handed to the platform layer as
     * bytes (§16). It was streamed with padding loops before; the total
     * size is already known here (shoff plus the section headers), so
     * building it costs one allocation and removes the last place the
     * object writer knew what a FILE* was. */
    size_t total = (size_t)shoff + (size_t)w->nsec * shentsize;
    unsigned char *img = xmalloc(total ? total : 1);
    memset(img, 0, total);            /* the gaps ARE zero padding */
    if (w->elf32) {
        Elf32_Ehdr e32;
        memset(&e32, 0, sizeof e32);
        memcpy(e32.e_ident, eh.e_ident, EI_NIDENT);
        e32.e_type = eh.e_type;
        e32.e_machine = eh.e_machine;
        e32.e_version = eh.e_version;
        e32.e_shoff = (Elf32_Off)eh.e_shoff;
        e32.e_flags = eh.e_flags;
        e32.e_ehsize = eh.e_ehsize;
        e32.e_shentsize = eh.e_shentsize;
        e32.e_shnum = eh.e_shnum;
        e32.e_shstrndx = eh.e_shstrndx;
        memcpy(img, &e32, sizeof e32);
    } else {
        memcpy(img, &eh, sizeof eh);
    }
    for (int i = 1; i < w->nsec; i++) {
        struct section *s = &w->sec[i];
        if (s->hdr.sh_type == SHT_NOBITS)
            continue;
        memcpy(img + s->hdr.sh_offset, s->data.p, s->data.len);
    }
    for (int i = 0; i < w->nsec; i++) {
        unsigned char *at = img + (size_t)shoff + (size_t)i * shentsize;
        if (w->elf32) {
            const Elf64_Shdr *h = &w->sec[i].hdr;
            Elf32_Shdr h32;
            h32.sh_name = h->sh_name;
            h32.sh_type = h->sh_type;
            h32.sh_flags = (Elf32_Word)h->sh_flags;
            h32.sh_addr = (Elf32_Addr)h->sh_addr;
            h32.sh_offset = (Elf32_Off)h->sh_offset;
            h32.sh_size = (Elf32_Word)h->sh_size;
            h32.sh_link = h->sh_link;
            h32.sh_info = h->sh_info;
            h32.sh_addralign = (Elf32_Word)h->sh_addralign;
            h32.sh_entsize = (Elf32_Word)h->sh_entsize;
            memcpy(at, &h32, sizeof h32);
        } else {
            memcpy(at, &w->sec[i].hdr, sizeof(Elf64_Shdr));
        }
    }
    int rc = plat_write_file(path, img, total);
    free(img);
    if (rc != 0) {
        fprintf(stderr, "embcc: cannot write '%s'\n", path);
        return -1;
    }
    return 0;
}
