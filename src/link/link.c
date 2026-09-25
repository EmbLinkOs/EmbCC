/* EmbLD — the integrated linker. See link.h.
 *
 * Structure, in load-sequence order (the read is the algorithm, the way
 * embread mirrors the EMBX §6 load): parse each object → collect its
 * allocated sections → resolve the global symbol table → lay the
 * sections out into the text and data segments at fixed vaddrs → apply
 * relocations → write the ET_EXEC.
 *
 * Correct-and-slow first (ARCHITECTURE §3): the symbol table is a linear
 * scan, which is fine for the bounded symbol set B1 pulls from libc.a; a
 * hash lands when a measured corpus makes it slow, not before.
 */
#include "link.h"
#include "../platform/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../elf/elf.h"
#include "../embx/embx.h"
#include "../../tools/embdbg/embdbg_core.h"

/* EmbLink app image (TARGET_ABI §4a, newlib.ld): text at 0x400000
 * (R+X), then a page boundary (W^X), then data (R+W). */
#define DEFAULT_BASE 0x400000ULL
#define PAGE 0x1000ULL

/* ---- input objects ---- */

struct object {
    const char *name;         /* for diagnostics: "libc.a(printf.o)" etc */
    unsigned char *buf;
    long len;
    Elf64_Ehdr *eh;
    Elf64_Shdr *shdrs;
    int nsh;
    const char *shstr;
    Elf64_Sym *syms;
    int nsym;
    const char *symstr;
    int local_syms;           /* sh_info of the symtab: [0,local) are LOCAL */
    /* per input section: index into insecs[], or -1 if not laid out */
    int *sec_out;
    /* ELFCLASS32 input (ARMv7-M, D-015). The headers and the symbol
     * table are CONVERTED into the 64-bit structures above at parse
     * time, so nothing downstream of parse_object knows: only the
     * relocation reader, which walks entries of a different size, and
     * the writer, which has to put the class back. */
    int elf32;
    int machine;              /* e_machine, checked to be one across inputs */
};

/* An allocated input section placed into the output. */
enum seg { SEG_TEXT, SEG_DATA };

/* Output sections, in layout order. Input sections are grouped by name
 * into these, so — the reason this exists — all .init_array inputs land
 * contiguously and the bracket symbols __init_array_start/_end are just
 * the group's bounds, the way a linker script's `*(.init_array)` places
 * them. Order matters: constructors precede ordinary data; .bss is last
 * (NOBITS, the memsz tail). */
enum osec {
    /* The interrupt vector table, FIRST because a Cortex-M does not
     * look it up -- it fetches the initial stack pointer from the word
     * at the image's base and the reset address from the next one. An
     * orphan group would place it after .rodata, where the processor
     * would read whatever happened to land at zero and branch there.
     * Empty and harmless on every other target. */
    OSEC_VECTORS,
    OSEC_TEXT, OSEC_RODATA,               /* text segment (R+X) */
    /* The thread block, first in the data segment. These are not data
     * every thread shares: they are the TEMPLATE each thread's private
     * copy is made from, and PT_TLS is what says so. They are placed
     * like ordinary sections so the image holds their bytes, and the
     * addresses a program uses for them are OFFSETS from a thread
     * pointer, computed in the TPOFF relocation below. */
    OSEC_TDATA, OSEC_TBSS,
    OSEC_INIT_ARRAY, OSEC_FINI_ARRAY,     /* data segment (R+W) */
    OSEC_CTORS, OSEC_DTORS,
    OSEC_DATA, OSEC_BSS,
    OSEC_COUNT
};

struct insec {
    struct object *obj;
    int shndx;
    const char *name;
    const unsigned char *data; /* NULL for NOBITS (.bss) */
    Elf64_Xword size;
    Elf64_Xword align;
    int is_bss;
    enum seg seg;
    int osec;
    Elf64_Addr vaddr;          /* assigned in layout */
};

/* The final [start,end) vaddr span of each output section — the source
 * of the bracket symbols. */
struct osec_bound { Elf64_Addr start, end; };

/* A section name none of the fixed groups claims (.embk_exports, a
 * `section("my_table")` array) is an orphan, and each distinct orphan
 * name is a group of its own, numbered from OSEC_COUNT: its inputs land
 * contiguously, the way `KEEP(*(.embk_exports))` places them, and its
 * bounds become bracket symbols (define_orphan_brackets). Read-only
 * orphans follow .rodata; writable ones follow .data. */
#define MAX_ORPHANS 64
struct orphan { const char *name; int writable; };

struct symbol {
    const char *name;
    struct object *obj;        /* defining object, or NULL if undefined */
    int insec;                 /* insecs index of its section, or -1 (ABS) */
    Elf64_Addr value;          /* section-relative until layout, then absolute */
    int defined;
    int weak;
    int common;                /* a tentative (COMMON) definition */
    Elf64_Xword size;          /* for COMMON: the size to reserve */
    Elf64_Xword align;         /* for COMMON */
};

/* A static archive (.a) is a pool of member objects; a member is pulled
 * into the link only if it defines a symbol something still needs
 * (classic archive semantics, TARGET_ABI §4b). */
struct member {
    const char *name;         /* "libc.a(malloc.o)" for diagnostics */
    unsigned char *buf;
    long len;
    int pulled;
    struct object *obj;       /* parsed lazily on first inspection */
};

struct archive {
    const char *name;
    struct member *members;
    int nmembers;
};

struct linker {
    /* The thread block, for the TPOFF relocations: where it starts in
     * the image, and its aligned size -- which is what an offset is
     * measured back from, because x86-64 puts the block below the
     * thread pointer. */
    Elf64_Addr tls_start;
    Elf64_Xword tls_size;
    struct object **objs;
    int nobj, capobj;
    struct insec *insecs;
    int nsec, capsec;
    struct symbol *syms;
    int nsym, capsym;
    struct archive **archives;
    int narch, caparch;
    Elf64_Addr base;
    const char *entry;
    Elf64_Addr lma_offset;     /* L2: p_paddr = p_vaddr - this (0 = paddr==vaddr) */
    Elf64_Addr data_base;      /* firmware: the writable segment's VMA (0 = off) */
    Elf64_Addr data_lma;       /* ... and where its bytes are STORED */
    int elf32;                 /* ELFCLASS32 output, from the inputs */
    int machine;               /* e_machine, one across every input */
    struct orphan orphans[MAX_ORPHANS];
    int norphan;
};

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "embld: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fatal_unwind();
}

/* ---- symbol table (linear; §3 correct-and-slow) ---- */

static struct symbol *sym_find(struct linker *l, const char *name)
{
    for (int i = 0; i < l->nsym; i++)
        if (strcmp(l->syms[i].name, name) == 0)
            return &l->syms[i];
    return NULL;
}

static struct symbol *sym_intern(struct linker *l, const char *name)
{
    struct symbol *s = sym_find(l, name);
    if (s)
        return s;
    if (l->nsym == l->capsym) {
        l->capsym = l->capsym ? l->capsym * 2 : 64;
        l->syms = xrealloc(l->syms, (size_t)l->capsym * sizeof *l->syms);
    }
    s = &l->syms[l->nsym++];
    memset(s, 0, sizeof *s);
    s->name = name;
    s->insec = -1;
    return s;
}

/* ---- object parsing ---- */

static Elf64_Shdr *sh_at(struct object *o, int i) { return &o->shdrs[i]; }

static struct object *parse_object(const char *name, unsigned char *buf,
                                   long len)
{
    if (len < (long)sizeof(Elf64_Ehdr))
        die("%s: too small to be an object", name);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3)
        die("%s: not an ELF file", name);
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
        die("%s: not little-endian", name);
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 &&
        eh->e_ident[EI_CLASS] != ELFCLASS32)
        die("%s: not a 32- or 64-bit ELF", name);

    struct object *o = xcalloc(1, sizeof *o);
    o->name = name;
    o->buf = buf;
    o->len = len;
    o->elf32 = eh->e_ident[EI_CLASS] == ELFCLASS32;

    /* The 32-bit case is read into the 64-bit structures the rest of
     * this file uses. Field by field, because Elf32_Shdr and Elf32_Sym
     * are not their 64-bit namesakes with narrower members -- Elf32_Sym
     * puts st_value and st_size BEFORE st_info, where Elf64_Sym puts
     * them after. The section DATA stays where it is and is still read
     * out of o->buf; only the descriptions are copied. */
    if (o->elf32) {
        Elf32_Ehdr *e32 = (Elf32_Ehdr *)buf;
        if (e32->e_type != ET_REL)
            die("%s: not a relocatable object (ET_REL)", name);
        if (e32->e_machine != EM_ARM)
            die("%s: a 32-bit object for machine %u; only ARM (EM_ARM) "
                "is supported", name, (unsigned)e32->e_machine);
        o->machine = e32->e_machine;
        o->nsh = e32->e_shnum;
        if ((long)e32->e_shoff + (long)o->nsh * (long)sizeof(Elf32_Shdr) > len)
            die("%s: section headers run past end of file", name);
        o->eh = xcalloc(1, sizeof *o->eh);
        o->eh->e_type = e32->e_type;
        o->eh->e_machine = e32->e_machine;
        o->eh->e_shnum = e32->e_shnum;
        o->eh->e_shstrndx = e32->e_shstrndx;
        o->shdrs = xcalloc((size_t)(o->nsh ? o->nsh : 1), sizeof *o->shdrs);
        {
            Elf32_Shdr *s32 = (Elf32_Shdr *)(buf + e32->e_shoff);
            for (int i = 0; i < o->nsh; i++) {
                o->shdrs[i].sh_name      = s32[i].sh_name;
                o->shdrs[i].sh_type      = s32[i].sh_type;
                o->shdrs[i].sh_flags     = s32[i].sh_flags;
                o->shdrs[i].sh_addr      = s32[i].sh_addr;
                o->shdrs[i].sh_offset    = s32[i].sh_offset;
                o->shdrs[i].sh_size      = s32[i].sh_size;
                o->shdrs[i].sh_link      = s32[i].sh_link;
                o->shdrs[i].sh_info      = s32[i].sh_info;
                o->shdrs[i].sh_addralign = s32[i].sh_addralign;
                o->shdrs[i].sh_entsize   = s32[i].sh_entsize;
            }
        }
    } else {
        if (eh->e_type != ET_REL)
            die("%s: not a relocatable object (ET_REL)", name);
        if (eh->e_machine != EM_X86_64)
            die("%s: not x86-64", name);
        o->machine = eh->e_machine;
        o->eh = eh;
        o->nsh = eh->e_shnum;
        o->shdrs = (Elf64_Shdr *)(buf + eh->e_shoff);
        if (eh->e_shoff + (Elf64_Off)o->nsh * sizeof(Elf64_Shdr) >
            (Elf64_Off)len)
            die("%s: section headers run past end of file", name);
    }
    o->shstr = (const char *)(buf + o->shdrs[o->eh->e_shstrndx].sh_offset);
    o->sec_out = xmalloc((size_t)o->nsh * sizeof(int));
    for (int i = 0; i < o->nsh; i++)
        o->sec_out[i] = -1;

    /* find the symbol table */
    for (int i = 0; i < o->nsh; i++) {
        if (o->shdrs[i].sh_type == SHT_SYMTAB) {
            Elf64_Shdr *sh = &o->shdrs[i];
            o->local_syms = (int)sh->sh_info;
            o->symstr = (const char *)(buf + o->shdrs[sh->sh_link].sh_offset);
            if (o->elf32) {
                Elf32_Sym *s32 = (Elf32_Sym *)(buf + sh->sh_offset);
                o->nsym = (int)(sh->sh_size / sizeof(Elf32_Sym));
                o->syms = xcalloc((size_t)(o->nsym ? o->nsym : 1),
                                  sizeof *o->syms);
                for (int k = 0; k < o->nsym; k++) {
                    o->syms[k].st_name  = s32[k].st_name;
                    o->syms[k].st_info  = s32[k].st_info;
                    o->syms[k].st_other = s32[k].st_other;
                    o->syms[k].st_shndx = s32[k].st_shndx;
                    o->syms[k].st_value = s32[k].st_value;
                    o->syms[k].st_size  = s32[k].st_size;
                }
            } else {
                o->syms = (Elf64_Sym *)(buf + sh->sh_offset);
                o->nsym = (int)(sh->sh_size / sizeof(Elf64_Sym));
            }
            break;
        }
    }
    return o;
}

/* Which output section a named input section joins. Matched by prefix so
 * .text.foo joins .text, .data.rel joins .data, and so on — exactly the
 * grouping a linker script's wildcards express. */
static int classify_osec(const char *name, int writable, int is_bss)
{
    static const struct { const char *pfx; int osec; } map[] = {
        { ".init_array", OSEC_INIT_ARRAY },
        { ".fini_array", OSEC_FINI_ARRAY },
        { ".ctors", OSEC_CTORS },
        { ".dtors", OSEC_DTORS },
        /* Both spellings: `.vectors` and CMSIS's `.isr_vector`. */
        { ".vectors", OSEC_VECTORS },
        { ".isr_vector", OSEC_VECTORS },
        { ".text", OSEC_TEXT },
        { ".rodata", OSEC_RODATA },
        { ".data", OSEC_DATA },
        { ".bss", OSEC_BSS },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        size_t n = strlen(map[i].pfx);
        if (strncmp(name, map[i].pfx, n) == 0 &&
            (name[n] == 0 || name[n] == '.'))
            return map[i].osec;
    }
    /* an unrecognized allocated section: NOBITS joins .bss; anything else
     * is an orphan group (orphan_osec) */
    (void)writable;
    return is_bss ? OSEC_BSS : -1;
}

/* The group number of the orphan section `name`, registering it on first
 * sight. First-seen order is layout order, as with any input. */
static int orphan_osec(struct linker *l, const char *name, int writable)
{
    for (int i = 0; i < l->norphan; i++)
        if (strcmp(l->orphans[i].name, name) == 0)
            return OSEC_COUNT + i;
    if (l->norphan == MAX_ORPHANS)
        die("more than %d distinct orphan sections (at '%s')",
            MAX_ORPHANS, name);
    l->orphans[l->norphan].name = name;
    l->orphans[l->norphan].writable = writable;
    return OSEC_COUNT + l->norphan++;
}

/* Collect the object's SHF_ALLOC sections into the global insec list and
 * record where each landed (sec_out), so relocations and symbols can map
 * a (object, section) back to its output placement. */
static void collect_sections(struct linker *l, struct object *o)
{
    for (int i = 0; i < o->nsh; i++) {
        Elf64_Shdr *sh = sh_at(o, i);
        if (!(sh->sh_flags & SHF_ALLOC))
            continue;
        if (l->nsec == l->capsec) {
            l->capsec = l->capsec ? l->capsec * 2 : 64;
            l->insecs = xrealloc(l->insecs,
                                 (size_t)l->capsec * sizeof *l->insecs);
        }
        struct insec *s = &l->insecs[l->nsec];
        memset(s, 0, sizeof *s);
        s->obj = o;
        s->shndx = i;
        s->name = o->shstr + sh->sh_name;
        s->size = sh->sh_size;
        s->align = sh->sh_addralign ? sh->sh_addralign : 1;
        s->is_bss = sh->sh_type == SHT_NOBITS;
        s->data = s->is_bss ? NULL : o->buf + sh->sh_offset;
        /* SHF_TLS decides this, not the name. A .tbss is SHT_NOBITS,
         * so classifying by name and type alone would file it under
         * .bss -- where every thread would SHARE it, which is the
         * opposite of what a thread-local is. */
        if (sh->sh_flags & SHF_TLS)
            s->osec = s->is_bss ? OSEC_TBSS : OSEC_TDATA;
        else
            s->osec = classify_osec(s->name, sh->sh_flags & SHF_WRITE,
                                    s->is_bss);
        if (s->osec < 0)
            s->osec = orphan_osec(l, s->name, !!(sh->sh_flags & SHF_WRITE));
        /* text segment: executable OR read-only allocatable (.rodata);
         * data segment: writable and the constructor arrays. W^X by
         * construction — the constructor arrays are read-only data that
         * the ABI keeps in the writable segment (they hold relocated
         * pointers), never executable. */
        s->seg = (s->osec == OSEC_VECTORS ||
                  s->osec == OSEC_TEXT || s->osec == OSEC_RODATA ||
                  (s->osec >= OSEC_COUNT &&
                   !l->orphans[s->osec - OSEC_COUNT].writable))
                     ? SEG_TEXT : SEG_DATA;
        o->sec_out[i] = l->nsec;
        l->nsec++;
    }
}

static void add_symbols(struct linker *l, struct object *o);

/* Bring a parsed object fully into the link: register it, lay out its
 * allocated sections, and merge its symbols. */
static void add_object(struct linker *l, struct object *o)
{
    /* One machine per link, decided by the first object. Mixing them is
     * not a case to handle later: the relocations, the pointer width
     * and the output class all follow from it, and an image containing
     * both would be neither. */
    if (l->nobj == 0) {
        l->machine = o->machine;
        l->elf32 = o->elf32;
    } else if (o->machine != l->machine) {
        die("%s: an object for a different machine than the ones before "
            "it (%u against %u)", o->name, (unsigned)o->machine,
            (unsigned)l->machine);
    }
    if (l->nobj == l->capobj) {
        l->capobj = l->capobj ? l->capobj * 2 : 8;
        l->objs = xrealloc(l->objs, (size_t)l->capobj * sizeof *l->objs);
    }
    l->objs[l->nobj++] = o;
    collect_sections(l, o);
    add_symbols(l, o);
}

/* ---- static archives (ar format) ---- */

/* The System V / GNU ar header before each member — 60 bytes, ASCII
 * decimal fields. The member name is what makes it fiddly: a short name
 * is "name/" (trailing slash); a long name is "/offset" into the "//"
 * string-table member. The "/" and "//" members are the symbol index
 * and the string table — skipped, because members are scanned directly. */
struct ar_hdr {
    char name[16];
    char mtime[12];
    char uid[6];
    char gid[6];
    char mode[8];
    char size[10];
    char end[2];              /* "`\n" */
};

static long ar_num(const char *p, int n)
{
    long v = 0;
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++)
        v = v * 10 + (p[i] - '0');
    return v;
}

static int is_archive(const unsigned char *buf, long len)
{
    return len >= 8 && memcmp(buf, "!<arch>\n", 8) == 0;
}

/* Parses an archive into its member list. Long names resolve through the
 * "//" string table; the "/"/"" symbol-index member is skipped (members
 * are inspected directly at pull time). */
static void parse_archive(struct linker *l, const char *name,
                          unsigned char *buf, long len)
{
    struct archive *ar = xcalloc(1, sizeof *ar);
    ar->name = name;
    const char *longnames = NULL;

    long off = 8; /* past "!<arch>\n" */
    while (off + (long)sizeof(struct ar_hdr) <= len) {
        struct ar_hdr *h = (struct ar_hdr *)(buf + off);
        if (h->end[0] != '`' || h->end[1] != '\n')
            die("%s: corrupt archive header at offset %ld", name, off);
        long msize = ar_num(h->size, 10);
        long data = off + sizeof(struct ar_hdr);

        /* member name */
        char mname[256];
        if (h->name[0] == '/' && h->name[1] == '/') {
            /* the long-name string table */
            longnames = (const char *)(buf + data);
            mname[0] = 0;
        } else if (h->name[0] == '/' &&
                   (h->name[1] == ' ' || h->name[1] == 0)) {
            mname[0] = 0; /* the symbol index — skipped */
        } else if (h->name[0] == '/') {
            long noff = ar_num(h->name + 1, 15);
            const char *s = longnames ? longnames + noff : "?";
            int k = 0;
            while (s[k] && s[k] != '/' && s[k] != '\n' && k < 255) {
                mname[k] = s[k];
                k++;
            }
            mname[k] = 0;
        } else {
            int k = 0;
            while (k < 16 && h->name[k] && h->name[k] != '/' &&
                   h->name[k] != ' ') {
                mname[k] = h->name[k];
                k++;
            }
            mname[k] = 0;
        }

        if (mname[0]) { /* a real object member */
            if (ar->nmembers % 64 == 0)
                ar->members = xrealloc(ar->members,
                    (size_t)(ar->nmembers + 64) * sizeof *ar->members);
            struct member *m = &ar->members[ar->nmembers++];
            memset(m, 0, sizeof *m);
            size_t nlen = strlen(name) + strlen(mname) + 4;
            char *full = xmalloc(nlen);
            snprintf(full, nlen, "%s(%s)", name, mname);
            m->name = full;
            m->buf = buf + data;
            m->len = msize;
        }
        off = data + msize;
        if (off & 1)
            off++; /* members are 2-byte aligned */
    }

    if (l->narch == l->caparch) {
        l->caparch = l->caparch ? l->caparch * 2 : 8;
        l->archives = xrealloc(l->archives,
                               (size_t)l->caparch * sizeof *l->archives);
    }
    l->archives[l->narch++] = ar;
}

/* Does this parsed object define a symbol that is currently referenced
 * but undefined? That is exactly the condition to pull an archive
 * member. */
static int defines_needed(struct linker *l, struct object *o)
{
    for (int i = o->local_syms; i < o->nsym; i++) {
        Elf64_Sym *sy = &o->syms[i];
        if (sy->st_shndx == SHN_UNDEF)
            continue;
        const char *nm = o->symstr + sy->st_name;
        if (!*nm)
            continue;
        struct symbol *g = sym_find(l, nm);
        if (g && !g->defined)
            return 1;
    }
    return 0;
}

/* Pull members to a fixed point: repeatedly, any not-yet-pulled member
 * that satisfies a still-undefined symbol is linked in — which may
 * create new undefined symbols an earlier member then satisfies, so the
 * scan repeats until a whole pass pulls nothing. This handles libc.a's
 * two-way dependencies (malloc↔sbrk, printf→malloc) without caring about
 * member order. */
static void pull_archives(struct linker *l)
{
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int a = 0; a < l->narch; a++) {
            struct archive *ar = l->archives[a];
            for (int m = 0; m < ar->nmembers; m++) {
                struct member *mem = &ar->members[m];
                if (mem->pulled)
                    continue;
                if (!mem->obj)
                    mem->obj = parse_object(mem->name, mem->buf, mem->len);
                if (!defines_needed(l, mem->obj))
                    continue;
                mem->pulled = 1;
                add_object(l, mem->obj);
                progress = 1;
            }
        }
    }
}

/* Add this object's global definitions and note its undefined references.
 * The resolution rule (static link): a strong definition wins; a second
 * strong definition of the same name is an error; a weak definition
 * yields to a strong one; COMMON (tentative) is superseded by any real
 * definition and merged with other COMMONs at the largest size. */
static void add_symbols(struct linker *l, struct object *o)
{
    for (int i = o->local_syms; i < o->nsym; i++) {
        Elf64_Sym *sy = &o->syms[i];
        const char *name = o->symstr + sy->st_name;
        if (!*name)
            continue;
        int bind = ELF64_ST_BIND(sy->st_info);
        int weak = bind == STB_WEAK;
        struct symbol *g = sym_intern(l, name);

        if (sy->st_shndx == SHN_UNDEF)
            continue; /* a reference; may be satisfied by a later object */

        if (sy->st_shndx == SHN_COMMON) {
            /* tentative definition: reserve space unless something real
             * defines it. Largest size, strongest alignment win. */
            if (g->defined && !g->common)
                continue; /* a real definition already supersedes it */
            g->common = 1;
            g->defined = 1;
            g->obj = o;
            if (sy->st_size > g->size)
                g->size = sy->st_size;
            if (sy->st_value > g->align) /* COMMON: st_value is alignment */
                g->align = sy->st_value;
            continue;
        }

        /* a real (allocated or ABS) definition */
        if (g->defined && !g->common && !g->weak && !weak)
            die("multiple definition of '%s' (in %s and %s)", name,
                g->obj ? g->obj->name : "?", o->name);
        if (g->defined && !g->weak && weak)
            continue; /* keep the strong one already present */

        g->defined = 1;
        g->common = 0;
        g->weak = weak;
        g->obj = o;
        g->value = sy->st_value;
        g->insec = (sy->st_shndx == SHN_ABS) ? -1
                                             : o->sec_out[sy->st_shndx];
    }
}

/* ---- layout ---- */

static Elf64_Addr align_up(Elf64_Addr v, Elf64_Xword a)
{
    if (a < 2)
        return v;
    return (v + a - 1) & ~(a - 1);
}

/* Places every insec belonging to output section `os`, recording the
 * group's [start,end) bounds. Advances *va. */
static void place_osec(struct linker *l, int os, Elf64_Addr *va,
                       struct osec_bound *b)
{
    /* the group starts where its first member does — the padding before
     * that member is not part of the group, or a bracket-walked table
     * would begin with it */
    for (int i = 0; i < l->nsec; i++)
        if (l->insecs[i].osec == os) {
            *va = align_up(*va, l->insecs[i].align);
            break;
        }
    b[os].start = *va;
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->osec != os)
            continue;
        *va = align_up(*va, s->align);
        s->vaddr = *va;
        *va += s->size;
    }
    b[os].end = *va;
}

/* Lay the output sections out in order into the two segments, recording
 * each group's bounds. COMMON (tentative) symbols are placed at the end
 * of .bss, since they have no input section of their own. */
static void layout(struct linker *l, struct osec_bound *b,
                   Elf64_Addr *text_start, Elf64_Xword *text_size,
                   Elf64_Addr *data_start, Elf64_Xword *data_filesz,
                   Elf64_Xword *data_memsz,
                   Elf64_Addr *tls_start, Elf64_Xword *tls_filesz,
                   Elf64_Xword *tls_memsz, Elf64_Xword *tls_align)
{
    Elf64_Addr va = l->base;

    *text_start = va;
    place_osec(l, OSEC_VECTORS, &va, b);
    place_osec(l, OSEC_TEXT, &va, b);
    place_osec(l, OSEC_RODATA, &va, b);
    for (int i = 0; i < l->norphan; i++)
        if (!l->orphans[i].writable)
            place_osec(l, OSEC_COUNT + i, &va, b);
    *text_size = va - *text_start;

    /* Where the writable segment is ADDRESSED. A firmware image says so
     * explicitly -- its RAM is nowhere near its flash -- and everything
     * else continues past the text at the W^X boundary. */
    if (l->data_base) {
        l->data_lma = align_up(va, 4);
        va = l->data_base;
    } else {
        va = align_up(va, PAGE);       /* W^X boundary */
    }
    *data_start = va;

    /* The thread block first in the data segment, and contiguous: the
     * template is copied as one run, so .tbss has to follow .tdata with
     * nothing between them.
     *
     * .tbss occupies no FILE space -- it is the zero tail of the
     * template -- so the file-backed part of the data segment must not
     * count it. That is why tls_filesz stops at the end of .tdata while
     * the location counter carries on: laying .tbss out as ordinary
     * NOBITS in the middle of the segment would leave a hole in the
     * file that everything after it was addressed past. */
    *tls_align = 1;
    for (int i = 0; i < l->nsec; i++)
        if ((l->insecs[i].osec == OSEC_TDATA ||
             l->insecs[i].osec == OSEC_TBSS) &&
            l->insecs[i].align > *tls_align)
            *tls_align = l->insecs[i].align;
    va = align_up(va, *tls_align);
    *tls_start = va;
    place_osec(l, OSEC_TDATA, &va, b);
    *tls_filesz = va - *tls_start;
    place_osec(l, OSEC_TBSS, &va, b);
    *tls_memsz = va - *tls_start;
    /* Everything after the template is addressed from where the FILE
     * bytes end, because .tbss contributed none. */
    va = *tls_start + *tls_filesz;

    place_osec(l, OSEC_INIT_ARRAY, &va, b);
    place_osec(l, OSEC_FINI_ARRAY, &va, b);
    place_osec(l, OSEC_CTORS, &va, b);
    place_osec(l, OSEC_DTORS, &va, b);
    place_osec(l, OSEC_DATA, &va, b);
    for (int i = 0; i < l->norphan; i++)
        if (l->orphans[i].writable)
            place_osec(l, OSEC_COUNT + i, &va, b);
    *data_filesz = va - *data_start;   /* .bss is beyond the file image */

    b[OSEC_BSS].start = va;
    place_osec(l, OSEC_BSS, &va, b);   /* real .bss inputs first */
    /* then COMMON: each tentative symbol gets space, largest alignment
     * honored, and its value fixed to the reserved slot. */
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *g = &l->syms[i];
        if (!g->common)
            continue;
        va = align_up(va, g->align ? g->align : 1);
        g->value = va;
        g->insec = -1;                 /* now an absolute address */
        g->common = 0;
        va += g->size;
    }
    b[OSEC_BSS].end = va;
    *data_memsz = va - *data_start;
}

/* Turn every section-relative symbol value into a final absolute vaddr,
 * now that every section has one. */
static void finalize_symbols(struct linker *l)
{
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *g = &l->syms[i];
        if (!g->defined || g->insec < 0)
            continue; /* insec == -1: ABS or already-placed COMMON */
        g->value += l->insecs[g->insec].vaddr;
    }
}

/* Define (or override a weak-undefined) linker symbol at an absolute
 * address. The __init_array_start/_end family are weak-undefined in
 * crt0 (which is why B1 linked with them at 0); defining them at the
 * real group bounds is what makes a program WITH constructors correct,
 * not just one whose .init_array happens to be empty. */
static void define_linker_symbol(struct linker *l, const char *name,
                                 Elf64_Addr value)
{
    struct symbol *g = sym_intern(l, name);
    /* only define it if something references it (it was interned) and it
     * is not already defined by a real object — a real definition wins */
    if (g->defined && g->insec >= 0)
        return;
    g->defined = 1;
    g->weak = 0;
    g->common = 0;
    g->insec = -1;
    g->value = value;
}

/* The bracket symbols crt0 walks, each pair the bounds of its group. An
 * empty group has start == end, so the walk does nothing — matching
 * cross-ld, which defines them even when empty. */
static void define_brackets(struct linker *l, const struct osec_bound *b)
{
    define_linker_symbol(l, "__init_array_start", b[OSEC_INIT_ARRAY].start);
    define_linker_symbol(l, "__init_array_end", b[OSEC_INIT_ARRAY].end);
    define_linker_symbol(l, "__fini_array_start", b[OSEC_FINI_ARRAY].start);
    define_linker_symbol(l, "__fini_array_end", b[OSEC_FINI_ARRAY].end);
    define_linker_symbol(l, "__ctors_start", b[OSEC_CTORS].start);
    define_linker_symbol(l, "__ctors_end", b[OSEC_CTORS].end);
    define_linker_symbol(l, "__dtors_start", b[OSEC_DTORS].start);
    define_linker_symbol(l, "__dtors_end", b[OSEC_DTORS].end);
}

static int is_c_ident(const char *s)
{
    if (!*s || (*s >= '0' && *s <= '9'))
        return 0;
    for (; *s; s++)
        if (!(*s == '_' || (*s >= 'a' && *s <= 'z') ||
              (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')))
            return 0;
    return 1;
}

/* Each orphan group's bounds, under both spellings a program reaches for:
 * GNU ld's __start_NAME/__stop_NAME when NAME is a C identifier, and for a
 * dotted .NAME the __NAME_start/__NAME_end the fixed groups above use
 * (.init_array -> __init_array_start) — which is what the EmbLinkOS kernel
 * scripts spell by hand for .embk_exports. */
static void define_orphan_brackets(struct linker *l,
                                   const struct osec_bound *b)
{
    for (int i = 0; i < l->norphan; i++) {
        const char *n = l->orphans[i].name;
        const struct osec_bound *ob = &b[OSEC_COUNT + i];
        char sym[256];
        if (is_c_ident(n) && strlen(n) < 200) {
            snprintf(sym, sizeof sym, "__start_%s", n);
            define_linker_symbol(l, sym, ob->start);
            snprintf(sym, sizeof sym, "__stop_%s", n);
            define_linker_symbol(l, sym, ob->end);
        } else if (n[0] == '.' && is_c_ident(n + 1) && strlen(n) < 200) {
            snprintf(sym, sizeof sym, "__%s_start", n + 1);
            define_linker_symbol(l, sym, ob->start);
            snprintf(sym, sizeof sym, "__%s_end", n + 1);
            define_linker_symbol(l, sym, ob->end);
        }
    }
}

/* L1: end-of-image symbols. A linker script's `kernel_end = .;` past .bss — the
 * kernel's PMM/VMM place the frame bitmap there — plus the standard `_end`/`end`
 * family. Auto-provided (define_linker_symbol only defines a referenced,
 * otherwise-undefined name), so ordinary programs are untouched and a real
 * definition still wins. `image_end` is the vaddr past the last (.bss) byte. */
static void define_end_symbols(struct linker *l, Elf64_Addr image_end)
{
    define_linker_symbol(l, "kernel_end", image_end);
    define_linker_symbol(l, "_end", image_end);
    define_linker_symbol(l, "end", image_end);
    define_linker_symbol(l, "__bss_end", image_end);
    define_linker_symbol(l, "__kernel_end", image_end);
}

/* What a firmware startup needs to bring RAM up, provided by the linker
 * so the startup can be ordinary C and no linker script has to be kept
 * in step with it:
 *
 *   for (p = __data_start, q = __data_load; p < __data_end; ) *p++ = *q++;
 *   for (p = __bss_start; p < __bss_end; ) *p++ = 0;
 *
 * Defined only when they are referenced and otherwise undefined
 * (define_linker_symbol's rule), so a hosted link is untouched and a
 * program that provides its own still wins. */
static void define_firmware_symbols(struct linker *l, Elf64_Addr data_start,
                                    Elf64_Xword data_filesz,
                                    Elf64_Xword data_memsz)
{
    define_linker_symbol(l, "__data_start", data_start);
    define_linker_symbol(l, "__data_end", data_start + data_filesz);
    define_linker_symbol(l, "__data_load", l->data_lma);
    define_linker_symbol(l, "__bss_start", data_start + data_filesz);
    (void)data_memsz;             /* __bss_end is define_end_symbols's */
}

/* Two libraries whose absence shows up HERE — at the link, as a bare
 * undefined name — rather than at the compile that caused it.
 *
 * The compiler runtime (libgcc's `__muldi3` family) is what a backend
 * calls when an operation has no instruction: 128-bit multiply and
 * divide, the shifts under them, complex multiplication, and on
 * aarch64 every `long double` operation there is. The unwinder
 * (`_Unwind_*`) is what `throw` uses to walk back up the stack. Both
 * exist on macOS and on EmbLinkOS because the platform supplies them,
 * and both are now `lib/rt` on the Linux targets.
 *
 * So these notes no longer say "missing": they say what the routine IS
 * and where it comes from, because a program that reaches one of them
 * has almost always lost librt.a off its link line. "undefined symbol
 * '__multi3'" is true and useless either way. Returns the note, or NULL
 * for an ordinary undefined symbol. */
static int ends_with(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static const char *missing_runtime_note(const char *name)
{
    static const char *const rt_suffix[] = {
        "ti3", "ti2", "di3", "di2", "si3", "si2",     /* integer */
        "sf2", "df2", "xf2", "tf2",                   /* conversions */
        "sc3", "dc3", "xc3", "tc3",                   /* complex */
        "sf3", "df3", "xf3", "tf3", NULL
    };
    int i;

    if (name[0] != '_')
        return NULL;
    if (strncmp(name, "_Unwind_", 8) == 0 ||
        strcmp(name, "__gxx_personality_v0") == 0 ||
        strcmp(name, "__register_frame_info") == 0 ||
        strcmp(name, "__deregister_frame_info") == 0 ||
        strcmp(name, "dl_iterate_phdr") == 0)
        return "this is the stack unwinder, which C++ exceptions need. "
               "EmbCC has one (lib/rt/unwind.c, in librt.a), so this "
               "usually means librt.a is not on the link line -- the "
               "driver puts it there by itself, and a hand-written link "
               "has to name it after libc.a";
    if (strncmp(name, "__", 2) != 0)
        return NULL;
    /* The conversion families are named by their two TYPES rather than
     * by a fixed suffix -- __fixtfti, __floatuntitf -- so they are
     * matched by prefix. Without this they fell through to a bare
     * undefined symbol, which is the message this whole function
     * exists to replace. */
    if (strncmp(name, "__fix", 5) == 0 || strncmp(name, "__float", 7) == 0 ||
        strncmp(name, "__trunc", 7) == 0 || strncmp(name, "__extend", 8) == 0)
        return "this is a compiler-runtime conversion between a "
               "floating-point type and an integer one, or between two "
               "floating-point widths — including every aarch64 `long "
               "double` operation, which is IEEE binary128 in software "
               "there. EmbCC ships these in librt.a (lib/rt); a link "
               "that reaches this note is usually one that left it out";
    for (i = 0; rt_suffix[i]; i++)
        if (ends_with(name, rt_suffix[i]))
            return "this is a compiler-runtime helper (libgcc's "
                   "__muldi3 family) — the routine a backend calls for "
                   "an operation the machine has no instruction for, "
                   "such as 128-bit multiply or divide. EmbCC ships "
                   "these in librt.a (lib/rt); the driver puts it on "
                   "the link line by itself, and a hand-written link "
                   "has to name it after libc.a";
    return NULL;
}

/* The absolute vaddr of a symbol referenced by a relocation. Undefined
 * weak binds to 0 (TARGET_ABI §4a). A strong undefined is a hard error:
 * the static link has no resolver to defer to. */
static Elf64_Addr reloc_symval(struct linker *l, struct object *o,
                               Elf64_Word symidx, int *is_undef_weak)
{
    Elf64_Sym *sy = &o->syms[symidx];
    const char *name = o->symstr + sy->st_name;
    *is_undef_weak = 0;

    if (ELF64_ST_BIND(sy->st_info) == STB_LOCAL) {
        /* a local symbol: resolve within this object directly */
        if (sy->st_shndx == SHN_ABS)
            return sy->st_value;
        int out = o->sec_out[sy->st_shndx];
        if (out < 0)
            die("%s: local symbol in a non-allocated section", o->name);
        return l->insecs[out].vaddr + sy->st_value;
    }

    struct symbol *g = *name ? sym_find(l, name) : NULL;
    if (g && g->defined)
        return g->value;
    if (ELF64_ST_BIND(sy->st_info) == STB_WEAK) {
        *is_undef_weak = 1;
        return 0;
    }
    {
        const char *note = missing_runtime_note(name);
        if (note)
            die("undefined symbol '%s' (referenced by %s)\n  note: %s",
                name, o->name, note);
    }
    die("undefined symbol '%s' (referenced by %s)", name, o->name);
    return 0;
}

/* ---- relocation ---- */

static void put32(unsigned char *p, unsigned int v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void put64(unsigned char *p, unsigned long long v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}
static unsigned int get16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static void put16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

/* ---- the ARM patches -------------------------------------------------
 *
 * A 32-bit Thumb instruction is two halfwords in program order, each
 * little-endian on its own -- not a little-endian word. Every function
 * here reads and writes them as halfwords for that reason.
 */

/* The ±16MB branch displacement shared by `bl` and `b.w`:
 * S:I1:I2:imm10:imm11, where I1 and I2 are STORED as J1 = ~(I1^S) and
 * J2 = ~(I2^S). That double negation exists so a short forward branch
 * has J1 = J2 = 1 and looks like the older ARM encoding, and it is the
 * single easiest thing in this relocation to get backwards. */
static void patch_thm_b24(struct object *o, unsigned char *loc, long long off)
{
    unsigned long v;
    unsigned s, i1, i2, j1, j2;
    if (off < -(1LL << 24) || off >= (1LL << 24))
        die("%s: a Thumb call is more than 16MB away; this linker mints "
            "no veneers", o->name);
    v = (unsigned long)(off >> 1) & 0xffffffUL;
    s = (unsigned)((v >> 23) & 1);
    i1 = (unsigned)((v >> 22) & 1);
    i2 = (unsigned)((v >> 21) & 1);
    j1 = (~(i1 ^ s)) & 1;
    j2 = (~(i2 ^ s)) & 1;
    put16(loc, 0xf000u | (s << 10) | (unsigned)((v >> 11) & 0x3ff));
    /* The second halfword's top nibble says which instruction this is
     * (0xd000 bl, 0x9000 b.w); it is kept, not rewritten. */
    put16(loc + 2, (get16(loc + 2) & 0xd000u) | (j1 << 13) | (j2 << 11) |
                   (unsigned)(v & 0x7ff));
}

static unsigned int get32loc(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* The displacement already encoded in a `bl` or `b.w`, undoing the
 * J1/J2 storage so it can serve as an implicit addend. */
static long long read_thm_b24(const unsigned char *loc)
{
    unsigned hi = get16(loc), lo = get16(loc + 2);
    unsigned s = (hi >> 10) & 1;
    unsigned j1 = (lo >> 13) & 1, j2 = (lo >> 11) & 1;
    unsigned i1 = (~(j1 ^ s)) & 1, i2 = (~(j2 ^ s)) & 1;
    unsigned long v = ((unsigned long)s << 23) | ((unsigned long)i1 << 22) |
                      ((unsigned long)i2 << 21) |
                      ((unsigned long)(hi & 0x3ff) << 11) |
                      (unsigned long)(lo & 0x7ff);
    long long off = (long long)(v << 1);
    if (off & (1LL << 24))                 /* sign-extend from 25 bits */
        off -= 1LL << 25;
    return off;
}

/* The 16-bit immediate a movw or movt already carries. */
static unsigned int read_thm_mov(const unsigned char *loc)
{
    unsigned hi = get16(loc), lo = get16(loc + 2);
    return ((hi & 0xfu) << 12) | (((hi >> 10) & 1u) << 11) |
           (((lo >> 12) & 7u) << 8) | (lo & 0xffu);
}

/* One half of a movw/movt pair: the 16-bit immediate is split across
 * imm4, i, imm3 and imm8, in that order and not in a contiguous field. */
static void patch_thm_mov(unsigned char *loc, unsigned int h)
{
    unsigned hi = get16(loc), lo = get16(loc + 2);
    hi = (hi & 0xfbf0u) | ((h >> 12) & 0xfu) | (((h >> 11) & 1u) << 10);
    lo = (lo & 0x8f00u) | (((h >> 8) & 7u) << 12) | (h & 0xffu);
    put16(loc, hi);
    put16(loc + 2, lo);
}

static void apply_relocs(struct linker *l, struct object *o)
{
    for (int i = 0; i < o->nsh; i++) {
        Elf64_Shdr *rsh = sh_at(o, i);
        /* SHT_REL as well as SHT_RELA. The ARM EABI specifies the
         * implicit-addend form, so every object a real ARM toolchain
         * produces carries .rel.text and not .rela.text -- and a
         * linker that skipped those would relocate nothing, link
         * without complaint, and produce an image that does nothing at
         * all. Which is exactly what happened. */
        int isrel = rsh->sh_type == SHT_REL;
        if (rsh->sh_type != SHT_RELA && !isrel)
            continue;
        int relsz = o->elf32 ? (isrel ? (int)sizeof(Elf32_Rel)
                                      : (int)sizeof(Elf32_Rela))
                             : (isrel ? (int)sizeof(Elf64_Rel)
                                      : (int)sizeof(Elf64_Rela));
        int target = (int)rsh->sh_info;         /* section being relocated */
        if (o->sec_out[target] < 0)
            continue; /* relocations for a non-allocated section (debug) */
        struct insec *ts = &l->insecs[o->sec_out[target]];
        if (ts->is_bss)
            continue;
        /* the output bytes to patch live in the object's own buffer; we
         * patch there, then copy the section into the image at write. */
        unsigned char *base = o->buf + sh_at(o, target)->sh_offset;
        unsigned char *rbytes = o->buf + rsh->sh_offset;
        int n = (int)(rsh->sh_size / (Elf64_Xword)relsz);

        for (int j = 0; j < n; j++) {
            Elf64_Addr r_offset;
            Elf64_Word type, symi;
            long long A;
            if (o->elf32) {
                Elf32_Rela *r32 = (Elf32_Rela *)(rbytes + (long)j * relsz);
                r_offset = r32->r_offset;
                /* ELF32 packs the symbol index into 24 bits and the type
                 * into 8 -- not the 32/32 split ELF64 uses. */
                type = r32->r_info & 0xff;
                symi = r32->r_info >> 8;
                A = isrel ? 0 : r32->r_addend;
            } else {
                Elf64_Rela *r64 = (Elf64_Rela *)(rbytes + (long)j * relsz);
                r_offset = r64->r_offset;
                type = ELF64_R_TYPE(r64->r_info);
                symi = ELF64_R_SYM(r64->r_info);
                A = r64->r_addend;
            }
            int uw;
            Elf64_Addr S = reloc_symval(l, o, symi, &uw);
            Elf64_Addr P = ts->vaddr + r_offset;      /* patch site vaddr */
            unsigned char *loc = base + r_offset;

            if (o->elf32) {
                /* SHT_REL keeps the addend IN the field, in whatever
                 * shape that field has -- a word for the data
                 * relocations, a branch displacement for a call, a
                 * 16-bit immediate split across four fields for a
                 * movw. Each is read back the way it was written. */
                if (isrel) {
                    switch (type) {
                    case R_ARM_ABS32:
                    case R_ARM_REL32:
                    case R_ARM_PREL31:
                        A = (int)get32loc(loc);
                        break;
                    case R_ARM_THM_CALL:
                    case R_ARM_THM_JUMP24:
                        /* The field encodes a displacement from P + 4,
                         * and the ABI's addend is measured from P — so
                         * the addend is four MORE than the field says.
                         * An assembler with nothing to point at writes
                         * a branch to itself (`f7ff fffe`, displacement
                         * -4), which is exactly how it spells an addend
                         * of zero; taking that -4 literally puts every
                         * call four bytes early, which is one halfword
                         * into the instruction before the one meant. */
                        A = read_thm_b24(loc) + 4;
                        break;
                    case R_ARM_THM_MOVW_ABS_NC:
                    case R_ARM_THM_MOVT_ABS:
                        A = (short)read_thm_mov(loc);
                        break;
                    default:
                        A = 0;
                        break;
                    }
                }
                switch (type) {
                case R_ARM_PREL31:
                    /* The exception index table's self-relative pointer:
                     * 31 bits of offset with the top bit preserved,
                     * which says whether the entry is a table offset or
                     * an inline unwind instruction. Nothing here reads
                     * those tables, but they are ALLOCATED, so the
                     * pointers in them must still be made consistent. */
                    put32(loc, (unsigned int)(((unsigned long)((long long)S +
                                A - (long long)P) & 0x7fffffffUL) |
                                (get32loc(loc) & 0x80000000UL)));
                    break;
                case R_ARM_ABS32:
                    /* S already carries the Thumb bit for a function
                     * symbol: the compiler and the assembler both put it
                     * in st_value, so nothing here adds it and nothing
                     * strips it. A vector table entry is exactly this. */
                    put32(loc, (unsigned int)(S + (Elf64_Addr)A));
                    break;
                case R_ARM_REL32:
                    put32(loc, (unsigned int)(long)((long long)S + A -
                                                    (long long)P));
                    break;
                case R_ARM_THM_CALL:
                case R_ARM_THM_JUMP24:
                    /* The displacement is between ADDRESSES, so the
                     * Thumb bit comes off S first -- leaving it on would
                     * shift every call by one byte. */
                    patch_thm_b24(o, loc,
                                  (long long)(S & ~(Elf64_Addr)1) + A -
                                  ((long long)P + 4));
                    break;
                case R_ARM_THM_MOVW_ABS_NC:
                    patch_thm_mov(loc, (unsigned int)((S + (Elf64_Addr)A)
                                                      & 0xffff));
                    break;
                case R_ARM_THM_MOVT_ABS:
                    patch_thm_mov(loc, (unsigned int)(((S + (Elf64_Addr)A)
                                                       >> 16) & 0xffff));
                    break;
                default:
                    die("%s: unsupported ARM relocation type %u (this is "
                        "the next linker increment, not a bug in your "
                        "program)", o->name, type);
                }
                continue;
            }

            switch (type) {
            case R_X86_64_64:
                put64(loc, (unsigned long long)(S + A));
                break;
            case R_X86_64_32:
            case R_X86_64_32S:
                put32(loc, (unsigned int)(S + A));
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32:
                /* TARGET_ABI §4a: PLT32 is a plain PC32 in a static
                 * link — no PLT slot is minted. */
                put32(loc, (unsigned int)(long)((long long)S + A -
                                                (long long)P));
                break;
            case R_X86_64_PC64:
                put64(loc, (unsigned long long)((long long)S + A -
                                                (long long)P));
                break;
            case R_X86_64_TPOFF32:
                /* Local-exec thread-local storage. S is the symbol's
                 * address in the TEMPLATE, so S - tls_start is its
                 * offset within the block, and the value the program
                 * needs is that offset minus the block's size -- a
                 * negative number, because the block sits below the
                 * thread pointer.
                 *
                 * A symbol with no thread block to belong to means an
                 * object was compiled with __thread and linked into an
                 * image that has no PT_TLS, which cannot be patched
                 * into something meaningful. */
                if (!l->tls_size)
                    die("%s: a thread-local relocation, but the image "
                        "has no thread block -- was a __thread object "
                        "linked without its .tdata/.tbss?", o->name);
                put32(loc, (unsigned int)(long)((long long)S + A -
                                                (long long)l->tls_start -
                                                (long long)l->tls_size));
                break;
            default:
                die("%s: unsupported relocation type %u (this is the "
                    "next linker increment, not a bug in your program)",
                    o->name, type);
            }
        }
    }
}

/* ---- output ---- */

static void write_exec(struct linker *l, const char *out,
                       Elf64_Addr entry,
                       Elf64_Addr text_start, Elf64_Xword text_size,
                       Elf64_Addr data_start, Elf64_Xword data_filesz,
                       Elf64_Xword data_memsz,
                       Elf64_Addr tls_start, Elf64_Xword tls_filesz,
                       Elf64_Xword tls_memsz, Elf64_Xword tls_align)
{
    /* A third program header when the image has a thread block, and
     * with it a second requirement that is easy to miss and fatal to
     * get wrong: the PROGRAM HEADERS THEMSELVES must be inside the
     * first loadable segment.
     *
     * The kernel tells a program where its headers are through AT_PHDR,
     * and can only do that if some PT_LOAD covers the file offset they
     * sit at. When none does it passes zero, and a program that needed
     * them -- which here means any program with a thread-local, since
     * PT_TLS is how the runtime finds its template -- gets nothing,
     * sets no thread pointer, and faults near address zero on its first
     * access. Nothing fails at link time and nothing fails at load
     * time.
     *
     * So an image WITH a thread block starts its first segment at file
     * offset 0, covering the headers. An image without one keeps the
     * layout it has always had, because EmbLinkOS's loader has been
     * reading that layout since before this existed and nothing here
     * needs to change for it. */
    int ntls = tls_memsz ? 1 : 0;
    int nph = 2 + ntls;
    /* File layout: ehdr, 2 phdrs, then the text bytes at a file offset
     * congruent to their vaddr mod PAGE, then the data bytes likewise.
     * The kernel loader maps PT_LOAD by (offset, vaddr, filesz, memsz);
     * keeping offset ≡ vaddr (mod PAGE) is what lets it map file pages
     * directly. */
    /* A firmware image is not mapped by a kernel: it is COPIED into
     * flash, so there is no page-congruence to preserve and every
     * page of padding is flash the board does not have. Four bytes is
     * all its segments need to be aligned to. */
    Elf64_Xword pagesz = l->data_base ? 4 : PAGE;
    Elf64_Off ehsz = l->elf32 ? sizeof(Elf32_Ehdr) : sizeof(Elf64_Ehdr);
    Elf64_Off phsz = l->elf32 ? sizeof(Elf32_Phdr) : sizeof(Elf64_Phdr);
    Elf64_Off hdrs = ehsz + (Elf64_Off)nph * phsz;

    Elf64_Off text_off = hdrs;
    /* keep text_off ≡ text_start (mod pagesz) */
    text_off = align_up(text_off, pagesz) + (text_start & (pagesz - 1));
    if (text_off < hdrs)
        text_off += pagesz;
    Elf64_Off data_off = text_off + text_size;
    data_off = l->data_base
        ? align_up(data_off, 4)
        : align_up(data_off, pagesz) + (data_start & (pagesz - 1));

    Elf64_Off total = data_off + data_filesz;
    unsigned char *img = xcalloc(1, (size_t)total);

    /* Built as the 64-bit structures and written as whichever class the
     * inputs were, exactly as the object writer does it -- one place
     * that knows about the narrower layout instead of two shapes of
     * header threaded through everything above. */
    Elf64_Ehdr ehbuf;
    Elf64_Phdr phbuf[3];
    Elf64_Ehdr *eh = &ehbuf;
    memset(&ehbuf, 0, sizeof ehbuf);
    memset(phbuf, 0, sizeof phbuf);
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = l->elf32 ? ELFCLASS32 : ELFCLASS64;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_EXEC;              /* never ET_DYN — TARGET_ABI §4b */
    eh->e_machine = (Elf64_Half)(l->machine ? l->machine : EM_X86_64);
    eh->e_version = EV_CURRENT;
    /* The entry is a Thumb address on ARM and carries the low bit from
     * the symbol; the ELF entry must too, or the processor starts in
     * ARM state and faults on the first instruction. */
    eh->e_entry = entry;
    eh->e_flags = l->machine == EM_ARM ? EF_ARM_EABI_VER5 : 0;
    eh->e_phoff = ehsz;
    eh->e_ehsize = (Elf64_Half)ehsz;
    eh->e_phentsize = (Elf64_Half)phsz;
    eh->e_phnum = (Elf64_Half)nph;

    Elf64_Phdr *ph = phbuf;
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_X;
    if (ntls) {
        /* From file offset 0, so the headers are mapped and AT_PHDR is
         * real. The segment therefore begins text_off bytes before the
         * text does. */
        ph[0].p_offset = 0;
        ph[0].p_vaddr = text_start - text_off;
        ph[0].p_paddr = (text_start - text_off) - l->lma_offset;
        ph[0].p_filesz = text_off + text_size;
        ph[0].p_memsz = text_off + text_size;
    } else {
        ph[0].p_offset = text_off;
        ph[0].p_vaddr = text_start;
        ph[0].p_paddr = text_start - l->lma_offset; /* L2: higher-half */
        ph[0].p_filesz = text_size;
        ph[0].p_memsz = text_size;
    }
    ph[0].p_align = pagesz;
    ph[1].p_type = PT_LOAD;
    ph[1].p_flags = PF_R | PF_W;
    ph[1].p_offset = data_off;
    ph[1].p_vaddr = data_start;
    /* A firmware image's writable segment is STORED in flash, right
     * after the text, and ADDRESSED in RAM. That is the one case where
     * p_paddr is not p_vaddr shifted by a constant, which is why
     * lma_offset could not express it. */
    ph[1].p_paddr = l->data_base ? l->data_lma
                                 : data_start - l->lma_offset;  /* L2: LMA */
    ph[1].p_filesz = data_filesz;
    ph[1].p_memsz = data_memsz;       /* memsz > filesz = the .bss tail */
    ph[1].p_align = pagesz;

    /* The template, described rather than loaded twice: its bytes are
     * already inside the data segment above, and this header says which
     * of them they are. memsz exceeds filesz by the .tbss tail, which
     * each thread zeroes rather than copies. */
    if (ntls) {
        ph[2].p_type = PT_TLS;
        ph[2].p_flags = PF_R;
        ph[2].p_offset = data_off + (tls_start - data_start);
        ph[2].p_vaddr = tls_start;
        ph[2].p_paddr = tls_start - l->lma_offset;
        ph[2].p_filesz = tls_filesz;
        ph[2].p_memsz = tls_memsz;
        ph[2].p_align = tls_align;
    }

    if (l->elf32) {
        Elf32_Ehdr e32;
        memset(&e32, 0, sizeof e32);
        memcpy(e32.e_ident, eh->e_ident, EI_NIDENT);
        e32.e_type = eh->e_type;
        e32.e_machine = eh->e_machine;
        e32.e_version = eh->e_version;
        e32.e_entry = (Elf32_Addr)eh->e_entry;
        e32.e_phoff = (Elf32_Off)eh->e_phoff;
        e32.e_flags = eh->e_flags;
        e32.e_ehsize = eh->e_ehsize;
        e32.e_phentsize = eh->e_phentsize;
        e32.e_phnum = eh->e_phnum;
        memcpy(img, &e32, sizeof e32);
        for (int i = 0; i < nph; i++) {
            /* Elf32_Phdr is not Elf64_Phdr narrowed: p_flags moves from
             * second field to last. */
            Elf32_Phdr p32;
            p32.p_type = ph[i].p_type;
            p32.p_offset = (Elf32_Off)ph[i].p_offset;
            p32.p_vaddr = (Elf32_Addr)ph[i].p_vaddr;
            p32.p_paddr = (Elf32_Addr)ph[i].p_paddr;
            p32.p_filesz = (Elf32_Word)ph[i].p_filesz;
            p32.p_memsz = (Elf32_Word)ph[i].p_memsz;
            p32.p_flags = ph[i].p_flags;
            p32.p_align = (Elf32_Word)ph[i].p_align;
            memcpy(img + ehsz + (size_t)i * phsz, &p32, sizeof p32);
        }
    } else {
        memcpy(img, eh, sizeof *eh);
        memcpy(img + ehsz, ph, (size_t)nph * sizeof *ph);
    }

    /* copy each allocated, file-backed section to its place */
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->is_bss || !s->data)
            continue;
        Elf64_Off base = (s->seg == SEG_TEXT) ? text_off : data_off;
        Elf64_Addr segva = (s->seg == SEG_TEXT) ? text_start : data_start;
        memcpy(img + base + (s->vaddr - segva), s->data, (size_t)s->size);
    }

    if (plat_write_file(out, img, (size_t)total) != 0)
        die("cannot write '%s'", out);
    free(img);
}

/* Emit a native EMBX binary (EMBX_Specification_v2.md), the container ELF
 * cannot carry: it declares this program's required capabilities in a table
 * the kernel loader checks against the spawner's set (§6 step 9). Same two
 * W^X segments write_exec lays out — text R+X, data R+W — wrapped in EMBX's
 * header/segment/capability tables instead of an ELF ehdr+phdrs.
 *
 * Byte layout mirrors tools/embx/mkembx.py exactly so host and on-OS producers
 * agree: header(128) | 2 segment descriptors(64 each) | cap table(16 each) |
 * segment payloads at file offsets congruent to their vaddr mod align. */
static void emit_embx(struct linker *l, const char *out, unsigned long long caps,
                      Elf64_Addr entry,
                      Elf64_Addr text_start, Elf64_Xword text_size,
                      Elf64_Addr data_start, Elf64_Xword data_filesz,
                      Elf64_Xword data_memsz)
{
    /* Capability list: sorted ascending is free — walk cap_id low to high. */
    int ncaps = 0;
    for (int id = 1; id <= EMBX_CAP_MAX; id++)
        if (caps & (1ULL << id)) ncaps++;

    embx_u32 seg_tab_off = EMBX_HDR_SIZE;                    /* 128 */
    embx_u32 cap_tab_off = ncaps ? seg_tab_off + 2 * EMBX_SEG_SIZE : 0;
    embx_u32 tables_end  = seg_tab_off + 2 * EMBX_SEG_SIZE + (embx_u32)ncaps * EMBX_CAP_SIZE;

    /* Payload offsets: file_offset ≡ vaddr (mod PAGE), never before `cur`
     * (mkembx's congruent_offset; PAGE is a power of two). */
    embx_u64 text_fo = tables_end + (((embx_u64)text_start - tables_end) & (PAGE - 1));
    embx_u64 data_fo = (text_fo + text_size) + (((embx_u64)data_start - (text_fo + text_size)) & (PAGE - 1));
    embx_u64 image_size = data_fo + data_filesz;

    unsigned char *img = xcalloc(1, (size_t)image_size);

    /* --- section payloads: the same copy loop write_exec uses --- */
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->is_bss || !s->data) continue;
        embx_u64 base  = (s->seg == SEG_TEXT) ? text_fo : data_fo;
        Elf64_Addr segva = (s->seg == SEG_TEXT) ? text_start : data_start;
        memcpy(img + base + (s->vaddr - segva), s->data, (size_t)s->size);
    }

    /* --- header --- */
    struct embx_header *h = (struct embx_header *)img;
    const embx_u8 magic[8] = EMBX_MAGIC_BYTES;
    memcpy(h->magic, magic, 8);
    h->version_major = 1; h->version_minor = 0;
    h->header_size = EMBX_HDR_SIZE;
    h->binary_type = EMBX_TYPE_APP;
    h->machine = EMBX_MACHINE_X86_64;
    h->abi_version = EMBX_ABI_VERSION;
    h->flags = 0; h->feature_incompat = 0; h->feature_compat = 0;
    h->entry_point = entry;
    h->segment_table_offset = seg_tab_off;
    h->segment_count = 2;
    h->segment_entry_size = EMBX_SEG_SIZE;
    h->capability_table_offset = cap_tab_off;
    h->capability_count = (embx_u16)ncaps;
    h->capability_entry_size = EMBX_CAP_SIZE;
    h->grantor = 0; h->reserved0 = 0;
    h->image_size = image_size; h->reserved1 = 0;
    h->header_checksum = 0;                 /* filled last */

    /* --- segment descriptors: text R+X, data R+W (already W^X clean) --- */
    struct embx_segment *seg = (struct embx_segment *)(img + seg_tab_off);
    seg[0].type = EMBX_SEG_LOAD; seg[0].flags = EMBX_SEG_R | EMBX_SEG_X;
    seg[0].vaddr = text_start; seg[0].file_offset = text_fo;
    seg[0].file_size = text_size; seg[0].mem_size = text_size;
    seg[0].align = PAGE; seg[0].reserved0 = 0; seg[0].paddr = 0;
    seg[0].checksum = text_size ? embx_crc32c(img + text_fo, text_size) : 0;

    seg[1].type = EMBX_SEG_LOAD; seg[1].flags = EMBX_SEG_R | EMBX_SEG_W;
    seg[1].vaddr = data_start; seg[1].file_offset = data_fo;
    seg[1].file_size = data_filesz; seg[1].mem_size = data_memsz;
    seg[1].align = PAGE; seg[1].reserved0 = 0; seg[1].paddr = 0;
    seg[1].checksum = data_filesz ? embx_crc32c(img + data_fo, data_filesz) : 0;

    /* --- capability table (ascending, unique by construction) --- */
    if (ncaps) {
        struct embx_capability *ct = (struct embx_capability *)(img + cap_tab_off);
        int ci = 0;
        for (int id = 1; id <= EMBX_CAP_MAX; id++)
            if (caps & (1ULL << id)) {
                ct[ci].cap_id = (embx_u32)id;
                ct[ci].cap_flags = 0; ct[ci].reserved0 = 0;
                ci++;
            }
    }

    /* --- checksum order (§3.4): build_id over the whole image with build_id
     * and header_checksum still zero, then the header CRC32C last. --- */
    embdbg_sha256(img, (long)image_size, h->build_id);
    h->header_checksum = embx_crc32c(img, EMBX_HDR_BODY_SIZE);

    if (plat_write_file(out, img, (size_t)image_size) != 0)
        die("cannot write '%s'", out);
    free(img);
    fprintf(stderr, "embld: wrote %s (EMBX, %d capabilit%s)\n",
            out, ncaps, ncaps == 1 ? "y" : "ies");
}

static unsigned char *read_file(const char *path, long *len);

/* Emit a native .embdbg sidecar for a debug-carrying input, now that layout
 * has assigned final vaddrs. This is the link-time producer the format wants
 * (EMBDBG spec §2/§3: absolute vaddrs, build_id bound to the image). EmbCC
 * emits ET_REL objects with .text-relative debug addresses; the link is what
 * makes them absolute, so this belongs HERE, not in the compiler.
 *
 * Every directly-listed input object that carries .debug_line is merged into
 * one .embdbg, each biased by its own final .text vaddr. Archive members are
 * skipped — newlib's libc.a ships with debug info, but the intent of a -g link
 * is to debug YOUR objects, not incidentally-pulled libc members (this also
 * keeps a link with no -g object, like self-host, from emitting a sidecar). */
static void emit_embdbg(struct linker *l, const char *out)
{
    const unsigned char **objs = xmalloc((size_t)(l->nobj ? l->nobj : 1) * sizeof *objs);
    long *lens = xmalloc((size_t)(l->nobj ? l->nobj : 1) * sizeof *lens);
    long *biases = xmalloc((size_t)(l->nobj ? l->nobj : 1) * sizeof *biases);
    int n = 0;

    for (int i = 0; i < l->nobj; i++) {
        struct object *o = l->objs[i];
        if (strchr(o->name, '('))       /* "libc.a(member.o)" — an archive member */
            continue;
        int has_dbg = 0, text_idx = -1;
        for (int s = 0; s < o->nsh; s++) {
            const char *nm = o->shstr + o->shdrs[s].sh_name;
            if (strcmp(nm, ".debug_line") == 0) has_dbg = 1;
            if (strcmp(nm, ".text") == 0) text_idx = s;
        }
        if (!has_dbg) continue;
        Elf64_Addr tv = 0;
        if (text_idx >= 0 && o->sec_out[text_idx] >= 0)
            tv = l->insecs[o->sec_out[text_idx]].vaddr;
        objs[n] = o->buf; lens[n] = o->len; biases[n] = (long)tv;
        n++;
    }

    if (n) {
        long ilen;
        unsigned char *img = read_file(out, &ilen);
        char emb[4096];
        snprintf(emb, sizeof emb, "%s.embdbg", out);
        embdbg_emit_objects(objs, lens, biases, n, img, ilen, emb);
        free(img);
        fprintf(stderr, "embld: wrote %s (debug info from %d object%s)\n",
                emb, n, n == 1 ? "" : "s");
    }
    free(objs); free(lens); free(biases);
}

/* ---- driver ---- */

static unsigned char *read_file(const char *path, long *len)
{
    /* an object or an archive: an ordinary file, not a source */
    unsigned char *buf = (unsigned char *)plat_read_file(path, len);
    if (!buf)
        die("cannot read '%s'", path);
    return buf;
}

int embld_link(const char **inputs, int ninputs, const char *out,
               const struct link_opts *opts)
{
    struct linker l;
    memset(&l, 0, sizeof l);
    l.base = (opts && (opts->base || opts->have_base)) ? opts->base
                                                       : DEFAULT_BASE;
    l.entry = (opts && opts->entry) ? opts->entry : "_start";
    l.lma_offset = (opts) ? opts->lma_offset : 0;
    l.data_base = (opts) ? opts->data_base : 0;

    /* Explicit objects are always linked; archives are stashed and their
     * members pulled on demand (left-to-right, as a linker does — so an
     * archive satisfies references that appear before it on the line). */
    for (int i = 0; i < ninputs; i++) {
        long len;
        unsigned char *buf = read_file(inputs[i], &len);
        if (is_archive(buf, len)) {
            parse_archive(&l, inputs[i], buf, len);
            pull_archives(&l); /* satisfy what is undefined so far */
        } else {
            add_object(&l, parse_object(inputs[i], buf, len));
        }
    }
    /* A final fixed-point pass, so a later object's references can still
     * reach back into an earlier archive (the --start-group behaviour,
     * always on: correct over order-sensitive). */
    pull_archives(&l);

    Elf64_Addr text_start, data_start;
    Elf64_Xword text_size, data_filesz, data_memsz;
    struct osec_bound bounds[OSEC_COUNT + MAX_ORPHANS];
    memset(bounds, 0, sizeof bounds);
    Elf64_Addr tls_start = 0;
    Elf64_Xword tls_filesz = 0, tls_memsz = 0, tls_align = 1;
    layout(&l, bounds, &text_start, &text_size, &data_start, &data_filesz,
           &data_memsz, &tls_start, &tls_filesz, &tls_memsz, &tls_align);
    /* What a TPOFF relocation is measured against. x86-64 puts the
     * thread block BELOW the thread pointer, so an object at offset k
     * within the block is at tp - (aligned size) + k, and every such
     * relocation is negative. The runtime
     * (lib/libc/os/linux/tls.c) lays memory out to match, and rounds
     * to this same alignment -- rounding either side differently moves
     * the whole block and every access reads past its own variable. */
    l.tls_start = tls_start;
    l.tls_size = align_up(tls_memsz, tls_align);
    finalize_symbols(&l);
    define_brackets(&l, bounds);
    define_orphan_brackets(&l, bounds);
    define_end_symbols(&l, data_start + data_memsz);   /* L1: kernel_end/_end */
    define_firmware_symbols(&l, data_start, data_filesz, data_memsz);

    struct symbol *e = sym_find(&l, l.entry);
    if (!e || !e->defined)
        die("entry symbol '%s' is undefined", l.entry);

    for (int i = 0; i < l.nobj; i++)
        apply_relocs(&l, l.objs[i]);

    if (opts && opts->emit_embx) {
        emit_embx(&l, out, opts->caps, e->value, text_start, text_size,
                  data_start, data_filesz, data_memsz);
    } else {
        write_exec(&l, out, e->value, text_start, text_size,
                   data_start, data_filesz, data_memsz,
                   tls_start, tls_filesz, tls_memsz, tls_align);
        emit_embdbg(&l, out);   /* a .embdbg sidecar if any input carries -g info */
    }
    return 0;
}
