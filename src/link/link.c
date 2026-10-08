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
#include "../arch/riscv/emit.h"
#include "../arch/loongarch/emit.h"
#include "../arch/avr/emit.h"
#include "../arch/mips/emit.h"
#include "../arch/thumb/a32.h"
#include "../arch/tricore/emit.h"
#include "../arch/xtensa/emit.h"
#include "../arch/ppc/emit.h"
#include "../arch/rx/emit.h"
#include "../arch/sparc/emit.h"
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
    /* A big-endian (ELFDATA2MSB) object: its headers, symbols and
     * relocation entries were turned to host order in place when it was
     * read (be_normalise), its section CONTENTS were not -- they are the
     * target's, and the relocations patch them in its order. */
    int big_endian;
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
    /* Non-allocated .debug_* sections: which merged section each input
     * section joined (index into l->dbgsecs, or -1) and at what offset
     * within it. Kept beside sec_out because a relocation names an
     * input section index and has to reach one or the other. */
    int *dbg_sec;
    long *dbg_off;
    /* ELFCLASS32 input (ARMv7-M, D-015). The headers and the symbol
     * table are CONVERTED into the 64-bit structures above at parse
     * time, so nothing downstream of parse_object knows: only the
     * relocation reader, which walks entries of a different size, and
     * the writer, which has to put the class back. */
    int elf32;
    int machine;              /* e_machine, checked to be one across inputs */
    unsigned long eflags;     /* its e_flags */
    /* What .ARM.attributes says, plus one -- so 0 means the object did
     * not say, which is not the same as saying zero. An object with no
     * attributes section must not be read as claiming the base
     * standard. */
    int arm_vfp, arm_enum, arm_arch;
    /* An archive member: the symbol it was pulled in to define, and the
     * object that referred to it (the map file's first table). */
    const char *pulled_for;
    struct object *pulled_by;
    int *relsec;              /* --gc-sections: the REL/RELA section for
                               * each section, or -1 */
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
    /* A linker script's placement (-T): the index of the script output
     * section that claimed this input, -1 before any did, -2 for
     * /DISCARD/. The section's own flags decide where an unclaimed one
     * goes, as ld places an orphan. */
    int sosec;
    Elf64_Xword shflags;
    int discarded;
    /* --gc-sections: claimed by a KEEP() in the script (a root), and
     * reached by the mark. One the mark did not reach is `discarded`
     * like a /DISCARD/ one, and `gc` says that is why. */
    int keep, live, gc;
};

/* The final [start,end) vaddr span of each output section — the source
 * of the bracket symbols. */
struct osec_bound { Elf64_Addr start, end; };

/* A long-branch veneer (arm_veneers): for the calls from output section
 * `key` (a fixed group's index, or a linker script's output section) to
 * the global `gsym`, or to object `obj`'s local symbol `symi`, plus
 * `addend`; from ARM code (`arm`) or Thumb; `other` when the target is
 * in the other instruction state. Its twelve bytes are at 12 * `k` in
 * input section `sec`. */
struct veneer {
    int key, gsym, obj, symi, arm, other;
    long long addend;
    int sec, k;
};
struct veneer_sec { int key, sec, n; };

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
    int was_common;            /* a COMMON layout() placed: placed again */
    Elf64_Addr value;          /* section-relative until layout, then absolute */
    int defined;
    int weak;
    int common;                /* a tentative (COMMON) definition */
    int type;                  /* STT_FUNC/STT_OBJECT/..., from the input */
    Elf64_Xword size;          /* for COMMON: the size to reserve */
    Elf64_Xword align;         /* for COMMON */
    /* Defined by a linker script assignment rather than an object, and
     * then the script output section it was assigned in (-1: absolute). */
    int scripted;
    int sosec;
    /* The first object that referred to it: what an archive member was
     * pulled for, as the map file reports. */
    struct object *ref;
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

/* One merged .debug_* section. */
struct dbgsec {
    const char *name;
    unsigned char *data;
    long len, cap;
    int shndx;              /* its index in the output, filled at write */
};

struct linker {
    struct dbgsec *dbgsecs;
    int ndbg, capdbg;
    int keep_debug;         /* any input carried DWARF worth keeping */
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
    int *symhash;              /* sym_find's index: syms[] slot + 1 */
    int symhcap;
    struct archive **archives;
    int narch, caparch;
    Elf64_Addr base;
    const char *entry;
    /* RISC-V: what each PCREL_HI20 computed, keyed by the auipc's
     * address, for the low half that pairs with it. */
    struct { Elf64_Addr at; long long val; } *pcrel;
    int npcrel, cappcrel;
    Elf64_Addr stack_top;      /* RISC-V: the entry stub's sp, 0 = no stub */
    unsigned long csa_start, csa_end;   /* TriCore: the stub's CSA list */
    int stub_sec;              /* the stub's insecs index, or -1 */
    unsigned char *stub;       /* its bytes, written after layout */
    long stub_size;
    Elf64_Addr lma_offset;     /* L2: p_paddr = p_vaddr - this (0 = paddr==vaddr) */
    Elf64_Addr data_base;      /* firmware: the writable segment's VMA (0 = off) */
    Elf64_Addr data_lma;       /* ... and where its bytes are STORED */
    unsigned long rom_limit;   /* bytes of flash the image may occupy, 0 = any */
    int elf32;                 /* ELFCLASS32 output, from the inputs */
    int machine;               /* e_machine, one across every input */
    int big_endian;            /* the byte order, one across every input */
    const char *rel_sym;       /* the symbol the relocation being applied
                                * names, for need_range's message */
    int rel_uw;                /* ...and it is an undefined weak one, so
                                * its value is 0 by the link, not by its
                                * definition (apply_riscv) */
    /* The output's e_flags, from the inputs': RISC-V's EF_RISCV_RVC when
     * any of them has compressed code, AVR's architecture as the first
     * one names it. */
    unsigned long eflags;
    /* A Harvard machine: program space and data space are separate, and
     * no instruction reads read-only data where it was stored. .rodata
     * therefore belongs in the WRITABLE segment -- a RAM address with a
     * flash load address -- so the startup can copy it across. AVR is
     * the only such target here. */
    int harvard;
    /* MIPS: the floating-point ABI the first object's .MIPS.abiflags
     * named (0 = none named yet, or "any"), which every later one must
     * agree with -- a soft-float and a hard-float object pass doubles in
     * different registers. And which object named it, for the message. */
    int mips_fp_abi;
    const char *mips_fp_from;
    struct orphan orphans[MAX_ORPHANS];
    int norphan;
    struct ls_script *sc;      /* -T: the layout comes from here */
    /* ARMv8-M CMSE: the secure gateway veneers (cmse_scan) -- their
     * section's insecs index or -1, and for each the symbols it joins:
     * the entry's own name, which comes to name the veneer, and its
     * __acle_se_ twin, which keeps naming the code. */
    int sg_sec;
    struct { int sym, se; } *sg;
    int nsg;
    /* The ARM long-branch veneers (arm_veneers), and the section of each
     * output section's. */
    struct veneer *vn;
    int nvn, capvn;
    struct veneer_sec *vns;
    int nvns, capvns;
    int lb_thumb1;             /* the image is ARMv6-M: no MOVW/MOVT */
    int gc_sections;           /* --gc-sections */
    int print_gc;              /* --print-gc-sections */
    const char *const *gc_undefs;  /* -u: roots too */
    int gc_nundefs;
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

/* ---- symbol table ----
 *
 * Hashed. It was a linear scan, called for every relocation and, while
 * archives are searched, for every symbol of every member on every pass:
 * quadratic in the size of the link, which a firmware image built with
 * -ffunction-sections (a section and a relocation per call) feels first.
 * Open addressing over indices into syms[], so the table survives the
 * array being reallocated; a slot holds index+1, 0 being empty. */

static unsigned sym_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

static struct symbol *sym_find(struct linker *l, const char *name)
{
    if (!l->symhcap)
        return NULL;
    unsigned m = (unsigned)l->symhcap - 1, h = sym_hash(name) & m;
    for (int x; (x = l->symhash[h]) != 0; h = (h + 1) & m)
        if (strcmp(l->syms[x - 1].name, name) == 0)
            return &l->syms[x - 1];
    return NULL;
}

static void sym_hash_put(struct linker *l, int idx)
{
    unsigned m = (unsigned)l->symhcap - 1,
             h = sym_hash(l->syms[idx].name) & m;
    while (l->symhash[h])
        h = (h + 1) & m;
    l->symhash[h] = idx + 1;
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
    s->sosec = -1;
    if (2 * l->nsym > l->symhcap) {     /* at most half full */
        free(l->symhash);
        l->symhcap = l->symhcap ? 2 * l->symhcap : 256;
        l->symhash = xcalloc((size_t)l->symhcap, sizeof *l->symhash);
        for (int i = 0; i < l->nsym; i++)
            sym_hash_put(l, i);
    } else {
        sym_hash_put(l, l->nsym - 1);
    }
    return s;
}

#include "ldscript.h"

/* ---- object parsing ---- */

static Elf64_Shdr *sh_at(struct object *o, int i) { return &o->shdrs[i]; }


/* ---- ARM build attributes ---------------------------------------------
 *
 * Only the tags that decide whether two objects can be linked at all
 * are read; the rest are skipped, which the format allows because every
 * value is either a ULEB128 or a NUL-terminated string and the tag
 * number says which.
 */
enum { ARM_TAG_CPU_ARCH = 6, ARM_TAG_ENUM_SIZE = 26, ARM_TAG_VFP_ARGS = 28 };

/* The tags whose VALUE is a string rather than a number. From the ABI:
 * CPU_raw_name, CPU_name, compatibility, also_compatible_with and
 * conformance. A tag not in this list has a ULEB128 value. */
static int arm_tag_is_string(unsigned long t)
{
    return t == 4 || t == 5 || t == 32 || t == 65 || t == 67;
}

static unsigned long arm_uleb(const unsigned char **p, const unsigned char *end)
{
    unsigned long v = 0;
    int shift = 0;
    while (*p < end) {
        unsigned char b = *(*p)++;
        v |= (unsigned long)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    return v;
}

static void arm_attrs_scan(struct object *o,
                           const unsigned char *p, size_t n)
{
    const unsigned char *end = p + n;
    if (n < 1 || *p != 'A')
        return;                         /* not a format this knows */
    p++;
    while (p + 4 <= end) {
        unsigned long slen = (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
                             ((unsigned long)p[2] << 16) |
                             ((unsigned long)p[3] << 24);
        const unsigned char *sub = p + 4, *subend;
        if (slen < 4 || p + slen > end)
            return;
        subend = p + slen;
        /* the vendor string; only "aeabi" is defined */
        const char *vendor = (const char *)sub;
        while (sub < subend && *sub)
            sub++;
        if (sub < subend)
            sub++;
        if (strcmp(vendor, "aeabi") != 0) {
            p = subend;
            continue;                   /* a vendor nothing here knows */
        }
        while (sub + 5 <= subend) {
            unsigned char tag = *sub++;
            unsigned long blen = (unsigned long)sub[0] |
                                 ((unsigned long)sub[1] << 8) |
                                 ((unsigned long)sub[2] << 16) |
                                 ((unsigned long)sub[3] << 24);
            const unsigned char *b = sub + 4, *bend;
            if (blen < 5 || sub - 1 + blen > subend)
                return;
            bend = sub - 1 + blen;
            sub = bend;
            if (tag != 1)               /* only Tag_File is read */
                continue;
            while (b < bend) {
                unsigned long t = arm_uleb(&b, bend);
                if (arm_tag_is_string(t)) {
                    while (b < bend && *b)
                        b++;
                    if (b < bend)
                        b++;
                    continue;
                }
                unsigned long v = arm_uleb(&b, bend);
                if (t == ARM_TAG_VFP_ARGS)  { o->arm_vfp = (int)v + 1; }
                if (t == ARM_TAG_ENUM_SIZE) { o->arm_enum = (int)v + 1; }
                if (t == ARM_TAG_CPU_ARCH)  { o->arm_arch = (int)v + 1; }
            }
        }
        p = subend;
    }
}

/* Compare what each object says about itself, and refuse a combination
 * that cannot work. Called once every object is loaded, because the
 * question is about the SET and not about any one of them.
 *
 * A stored value is the tag's value plus one, so zero means "the object
 * did not say" -- an object with no attributes section at all, which
 * must not be treated as claiming the base standard. */
static void arm_attrs_check(struct linker *l)
{
    struct object *ref = NULL;
    for (int i = 0; i < l->nobj; i++) {
        struct object *o = l->objs[i];
        if (!o->arm_vfp && !o->arm_enum)
            continue;
        if (!ref) { ref = o; continue; }
        if (ref->arm_vfp && o->arm_vfp && ref->arm_vfp != o->arm_vfp)
            die("'%s' and '%s' disagree about where floating-point "
                "arguments go: one passes them in the core registers "
                "(-mfloat-abi=soft) and the other in s0-s15 "
                "(-mfloat-abi=hard). Linking them would leave every "
                "float argument read from a register the caller never "
                "wrote",
                ref->name ? ref->name : "?", o->name ? o->name : "?");
        if (ref->arm_enum && o->arm_enum && ref->arm_enum != o->arm_enum)
            die("'%s' and '%s' disagree about the size of an enum, which "
                "changes the layout of every struct that holds one",
                ref->name ? ref->name : "?", o->name ? o->name : "?");
    }
}


/* ---- big-endian objects ----------------------------------------------
 *
 * A big-endian ELF32 object (mips-none-elf) is read by turning its
 * DESCRIPTIONS -- the header, the section headers, the symbol table and
 * the relocation entries -- into host order in place, once, so the rest
 * of this file reads it like any other. The section contents stay in the
 * target's order: the relocations patch them through the machine's own
 * word accessors (mips_get_word), and the image is written in that order
 * with its headers swapped back (be_image). */
static void swap_n(void *field, size_t n)
{
    unsigned char *p = field;
    for (size_t k = 0; k < n / 2; k++) {
        unsigned char c = p[k];
        p[k] = p[n - 1 - k];
        p[n - 1 - k] = c;
    }
}
#define SWAP(f) swap_n(&(f), sizeof (f))

static void be_ehdr(Elf32_Ehdr *e)
{
    SWAP(e->e_type); SWAP(e->e_machine); SWAP(e->e_version);
    SWAP(e->e_entry); SWAP(e->e_phoff); SWAP(e->e_shoff); SWAP(e->e_flags);
    SWAP(e->e_ehsize); SWAP(e->e_phentsize); SWAP(e->e_phnum);
    SWAP(e->e_shentsize); SWAP(e->e_shnum); SWAP(e->e_shstrndx);
}

static void be_shdr(Elf32_Shdr *h)
{
    SWAP(h->sh_name); SWAP(h->sh_type); SWAP(h->sh_flags); SWAP(h->sh_addr);
    SWAP(h->sh_offset); SWAP(h->sh_size); SWAP(h->sh_link); SWAP(h->sh_info);
    SWAP(h->sh_addralign); SWAP(h->sh_entsize);
}

static void be_sym(Elf32_Sym *y)
{
    SWAP(y->st_name); SWAP(y->st_value); SWAP(y->st_size); SWAP(y->st_shndx);
}

static void be_phdr(Elf32_Phdr *p)
{
    SWAP(p->p_type); SWAP(p->p_offset); SWAP(p->p_vaddr); SWAP(p->p_paddr);
    SWAP(p->p_filesz); SWAP(p->p_memsz); SWAP(p->p_flags); SWAP(p->p_align);
}

static void be_normalise(const char *name, unsigned char *buf, long len)
{
    Elf32_Ehdr *e = (Elf32_Ehdr *)buf;
    Elf32_Shdr *sh;
    be_ehdr(e);
    if ((long)e->e_shoff + (long)e->e_shnum * (long)sizeof(Elf32_Shdr) > len)
        die("%s: section headers run past end of file", name);
    sh = (Elf32_Shdr *)(buf + e->e_shoff);
    for (int i = 0; i < e->e_shnum; i++)
        be_shdr(&sh[i]);
    for (int i = 0; i < e->e_shnum; i++) {
        unsigned char *p = buf + sh[i].sh_offset;
        if (sh[i].sh_type == SHT_NOBITS ||
            (long)sh[i].sh_offset + (long)sh[i].sh_size > len)
            continue;
        if (sh[i].sh_type == SHT_SYMTAB) {
            Elf32_Sym *y = (Elf32_Sym *)p;
            for (size_t k = 0; k < sh[i].sh_size / sizeof *y; k++)
                be_sym(&y[k]);
        } else if (sh[i].sh_type == SHT_REL) {
            Elf32_Rel *r = (Elf32_Rel *)p;
            for (size_t k = 0; k < sh[i].sh_size / sizeof *r; k++) {
                SWAP(r[k].r_offset); SWAP(r[k].r_info);
            }
        } else if (sh[i].sh_type == SHT_RELA) {
            Elf32_Rela *r = (Elf32_Rela *)p;
            for (size_t k = 0; k < sh[i].sh_size / sizeof *r; k++) {
                SWAP(r[k].r_offset); SWAP(r[k].r_info); SWAP(r[k].r_addend);
            }
        }
    }
}

/* The finished ELF32 image, written in host order, turned big-endian: the
 * header, the program and section headers, and the symbol table. */
static void be_image(unsigned char *img)
{
    Elf32_Ehdr *e = (Elf32_Ehdr *)img;
    Elf32_Shdr *sh = (Elf32_Shdr *)(img + e->e_shoff);
    Elf32_Phdr *ph = (Elf32_Phdr *)(img + e->e_phoff);
    int nsh = e->e_shnum, nph = e->e_phoff ? e->e_phnum : 0;
    for (int i = 0; i < nsh; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            Elf32_Sym *y = (Elf32_Sym *)(img + sh[i].sh_offset);
            for (size_t k = 0; k < sh[i].sh_size / sizeof *y; k++)
                be_sym(&y[k]);
        }
    }
    for (int i = 0; i < nsh; i++)
        be_shdr(&sh[i]);
    for (int i = 0; i < nph; i++)
        be_phdr(&ph[i]);
    e->e_ident[EI_DATA] = ELFDATA2MSB;
    be_ehdr(e);
}

/* ...and a big-endian ELF64 one (mips64-none-elf), the same way with the
 * 64-bit structures. */
static void be_ehdr64(Elf64_Ehdr *e)
{
    SWAP(e->e_type); SWAP(e->e_machine); SWAP(e->e_version);
    SWAP(e->e_entry); SWAP(e->e_phoff); SWAP(e->e_shoff); SWAP(e->e_flags);
    SWAP(e->e_ehsize); SWAP(e->e_phentsize); SWAP(e->e_phnum);
    SWAP(e->e_shentsize); SWAP(e->e_shnum); SWAP(e->e_shstrndx);
}

static void be_shdr64(Elf64_Shdr *h)
{
    SWAP(h->sh_name); SWAP(h->sh_type); SWAP(h->sh_flags); SWAP(h->sh_addr);
    SWAP(h->sh_offset); SWAP(h->sh_size); SWAP(h->sh_link); SWAP(h->sh_info);
    SWAP(h->sh_addralign); SWAP(h->sh_entsize);
}

static void be_sym64(Elf64_Sym *y)
{
    SWAP(y->st_name); SWAP(y->st_shndx); SWAP(y->st_value); SWAP(y->st_size);
}

static void be_phdr64(Elf64_Phdr *p)
{
    SWAP(p->p_type); SWAP(p->p_flags); SWAP(p->p_offset); SWAP(p->p_vaddr);
    SWAP(p->p_paddr); SWAP(p->p_filesz); SWAP(p->p_memsz); SWAP(p->p_align);
}

static void be_normalise64(const char *name, unsigned char *buf, long len)
{
    Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
    Elf64_Shdr *sh;
    be_ehdr64(e);
    if ((long)e->e_shoff + (long)e->e_shnum * (long)sizeof(Elf64_Shdr) > len)
        die("%s: section headers run past end of file", name);
    sh = (Elf64_Shdr *)(buf + e->e_shoff);
    for (int i = 0; i < e->e_shnum; i++)
        be_shdr64(&sh[i]);
    for (int i = 0; i < e->e_shnum; i++) {
        unsigned char *p = buf + sh[i].sh_offset;
        if (sh[i].sh_type == SHT_NOBITS ||
            (long)sh[i].sh_offset + (long)sh[i].sh_size > len)
            continue;
        if (sh[i].sh_type == SHT_SYMTAB) {
            Elf64_Sym *y = (Elf64_Sym *)p;
            for (size_t k = 0; k < sh[i].sh_size / sizeof *y; k++)
                be_sym64(&y[k]);
        } else if (sh[i].sh_type == SHT_RELA) {
            Elf64_Rela *r = (Elf64_Rela *)p;
            for (size_t k = 0; k < sh[i].sh_size / sizeof *r; k++) {
                SWAP(r[k].r_offset); SWAP(r[k].r_info); SWAP(r[k].r_addend);
            }
        } else if (sh[i].sh_type == SHT_REL) {
            Elf64_Rel *r = (Elf64_Rel *)p;
            for (size_t k = 0; k < sh[i].sh_size / sizeof *r; k++) {
                SWAP(r[k].r_offset); SWAP(r[k].r_info);
            }
        }
    }
}

/* The finished ELF64 image turned big-endian, as be_image does ELF32's. */
static void be_image64(unsigned char *img)
{
    Elf64_Ehdr *e = (Elf64_Ehdr *)img;
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + e->e_shoff);
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + e->e_phoff);
    int nsh = e->e_shnum, nph = e->e_phoff ? e->e_phnum : 0;
    for (int i = 0; i < nsh; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {
            Elf64_Sym *y = (Elf64_Sym *)(img + sh[i].sh_offset);
            for (size_t k = 0; k < sh[i].sh_size / sizeof *y; k++)
                be_sym64(&y[k]);
        }
    }
    for (int i = 0; i < nsh; i++)
        be_shdr64(&sh[i]);
    for (int i = 0; i < nph; i++)
        be_phdr64(&ph[i]);
    e->e_ident[EI_DATA] = ELFDATA2MSB;
    be_ehdr64(e);
}

/* ELFCLASS64 MIPS (n64): r_info is a record -- the symbol's 32 bits,
 * then r_ssym, r_type3, r_type2 and r_type, a byte each -- in the
 * object's byte order. Rewritten to ELF64_R_INFO(sym, type) once, so the
 * rest of this file reads it like any other; a COMPOSITE relocation (a
 * second or third type, which n64 PIC and gp-relative code use) is
 * refused by name. Called after be_normalise64 for a big-endian object,
 * which has already read the record as one big-endian word. */
static void mips64_rinfo(const char *name, unsigned char *buf, int big)
{
    Elf64_Ehdr *e = (Elf64_Ehdr *)buf;
    Elf64_Shdr *sh = (Elf64_Shdr *)(buf + e->e_shoff);
    for (int i = 0; i < e->e_shnum; i++) {
        size_t esz = sh[i].sh_type == SHT_RELA ? sizeof(Elf64_Rela)
                   : sh[i].sh_type == SHT_REL ? sizeof(Elf64_Rel) : 0;
        if (!esz)
            continue;
        for (size_t k = 0; k < sh[i].sh_size / esz; k++) {
            Elf64_Xword *ri = (Elf64_Xword *)(buf + sh[i].sh_offset +
                                              k * esz + 8);
            Elf64_Xword v = *ri;
            unsigned sym, ssym, t3, t2, t;
            if (big) {
                sym = (unsigned)(v >> 32);
                ssym = (unsigned)(v >> 24) & 0xff;
                t3 = (unsigned)(v >> 16) & 0xff;
                t2 = (unsigned)(v >> 8) & 0xff;
                t = (unsigned)v & 0xff;
            } else {
                sym = (unsigned)(v & 0xffffffffULL);
                ssym = (unsigned)(v >> 32) & 0xff;
                t3 = (unsigned)(v >> 40) & 0xff;
                t2 = (unsigned)(v >> 48) & 0xff;
                t = (unsigned)(v >> 56) & 0xff;
            }
            if (ssym || t2 || t3)
                die("%s: a composite MIPS64 relocation (type %u/%u/%u); "
                    "this linker links n64 relocations of one type each, "
                    "as non-PIC code without small data has them (compile "
                    "with -mno-abicalls -G0)", name, t, t2, t3);
            *ri = ELF64_R_INFO((Elf64_Xword)sym, (Elf64_Xword)t);
        }
    }
}

static struct object *parse_object(const char *name, unsigned char *buf,
                                   long len)
{
    int big = 0;
    if (len < (long)sizeof(Elf64_Ehdr))
        die("%s: too small to be an object", name);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3)
        die("%s: not an ELF file", name);
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 &&
        eh->e_ident[EI_CLASS] != ELFCLASS32)
        die("%s: not a 32- or 64-bit ELF", name);
    if (eh->e_ident[EI_DATA] == ELFDATA2MSB &&
        eh->e_ident[EI_CLASS] == ELFCLASS32) {
        /* big-endian: MIPS o32 and PowerPC, checked once it reads as
         * numbers */
        if (len < (long)sizeof(Elf32_Ehdr))
            die("%s: too small to be an object", name);
        be_normalise(name, buf, len);
        big = 1;
    } else if (eh->e_ident[EI_DATA] == ELFDATA2MSB) {
        /* big-endian ELF64: MIPS64 n64 only, checked below */
        be_normalise64(name, buf, len);
        big = 1;
    } else if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        die("%s: neither a little-endian object nor a big-endian one",
            name);
    }

    struct object *o = xcalloc(1, sizeof *o);
    o->name = name;
    o->buf = buf;
    o->len = len;
    o->elf32 = eh->e_ident[EI_CLASS] == ELFCLASS32;
    o->big_endian = big;

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
        if (e32->e_machine != EM_ARM && e32->e_machine != EM_RISCV &&
            e32->e_machine != EM_AVR && e32->e_machine != EM_MIPS &&
            e32->e_machine != EM_TRICORE && e32->e_machine != EM_XTENSA &&
            e32->e_machine != EM_PPC && e32->e_machine != EM_RX &&
            e32->e_machine != EM_SPARC && e32->e_machine != EM_68K)
            die("%s: a 32-bit object for machine %u; only ARM (EM_ARM), "
                "RV32 (EM_RISCV), AVR (EM_AVR), MIPS (EM_MIPS), TriCore "
                "(EM_TRICORE), Xtensa (EM_XTENSA), PowerPC (EM_PPC), RX (EM_RX) "
                "SPARC (EM_SPARC) and ColdFire (EM_68K) are supported", name,
                (unsigned)e32->e_machine);
        if (e32->e_machine == EM_TRICORE &&
            e32->e_ident[EI_DATA] != ELFDATA2LSB)
            die("%s: a big-endian TriCore object; TriCore is little-endian",
                name);
        if (e32->e_machine == EM_XTENSA &&
            e32->e_ident[EI_DATA] != ELFDATA2LSB)
            die("%s: a big-endian Xtensa object; this linker links "
                "little-endian Xtensa only", name);
        if (e32->e_machine == EM_RX &&
            (e32->e_ident[EI_DATA] != ELFDATA2LSB ||
             (e32->e_flags & E_FLAG_RX_64BIT_DOUBLES)))
            die("%s: an RX object that is big-endian or built with "
                "-m64bit-doubles; this linker links little-endian RX code "
                "with 32-bit doubles only", name);
        /* o32 only, in either byte order; and big-endian only there */
        if (e32->e_machine == EM_MIPS &&
            (e32->e_flags & 0x0000f000UL) != EF_MIPS_ABI_O32)
            die("%s: a MIPS object that is not o32; this linker links "
                "o32 (mipsel and mips) only", name);
        if (!big && e32->e_machine == EM_SPARC)
            die("%s: a little-endian SPARC object; SPARC is big-endian",
                name);
        if (big && e32->e_machine != EM_MIPS && e32->e_machine != EM_PPC &&
            e32->e_machine != EM_SPARC && e32->e_machine != EM_68K)
            die("%s: a big-endian object for machine %u; big-endian is "
                "linked for MIPS (mips-none-elf), PowerPC, SPARC and ColdFire "
                "only", name,
                (unsigned)e32->e_machine);
        if (!big && e32->e_machine == EM_PPC)
            die("%s: a little-endian PowerPC object; this linker links "
                "big-endian 32-bit PowerPC (powerpc-none-eabi) only", name);
        if (!big && e32->e_machine == EM_68K)
            die("%s: a little-endian m68k object; the m68k is big-endian",
                name);
        /* ColdFire: the ISA in e_flags' low nibble -- a 68000-family
         * object (68020, CPU32, ...) or one built for an FPU is not one
         * this linker's ColdFire images can run */
        if (e32->e_machine == EM_68K &&
            ((e32->e_flags & 0x0fUL) == 0 ||
             (e32->e_flags & 0x40UL) != 0))
            die("%s: an m68k object that is not soft-float ColdFire "
                "(e_flags 0x%lx); this linker links ColdFire ISA_A..C "
                "objects without the FPU", name,
                (unsigned long)e32->e_flags);
        o->machine = e32->e_machine;
        o->eflags = e32->e_flags;
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
        if (eh->e_machine != EM_X86_64 && eh->e_machine != EM_RISCV &&
            eh->e_machine != EM_LOONGARCH && eh->e_machine != EM_MIPS)
            die("%s: a 64-bit object for machine %u; only x86-64, RV64 "
                "(EM_RISCV), LoongArch64 (EM_LOONGARCH) and MIPS64 "
                "(EM_MIPS) are supported", name, (unsigned)eh->e_machine);
        if (big && eh->e_machine != EM_MIPS)
            die("%s: a big-endian 64-bit object for machine %u; big-endian "
                "is linked for MIPS (mips-none-elf, mips64-none-elf) only",
                name, (unsigned)eh->e_machine);
        /* n64 only: an ELFCLASS64 MIPS object is n64 unless it names an
         * ABI in e_flags (o64 or EABI64), which this linker does not */
        if (eh->e_machine == EM_MIPS) {
            if (eh->e_flags & 0x0000f000UL)
                die("%s: a 64-bit MIPS object that is not n64 (e_flags "
                    "0x%lx); this linker links n64 (mips64el and mips64) "
                    "only", name, (unsigned long)eh->e_flags);
            if (eh->e_shoff + (Elf64_Off)eh->e_shnum * sizeof(Elf64_Shdr) >
                (Elf64_Off)len)
                die("%s: section headers run past end of file", name);
            mips64_rinfo(name, buf, big);
        }
        /* LP64S only: an object for the FPU conventions passes a double
         * in a floating-point register where EmbCC's code passes it in
         * an integer one, and linking the two would be a miscompilation
         * made at link time. */
        if (eh->e_machine == EM_LOONGARCH &&
            (eh->e_flags & EF_LOONGARCH_ABI_MASK) !=
                EF_LOONGARCH_ABI_SOFT_FLOAT)
            die("%s: a LoongArch object for the %s-float ABI (e_flags "
                "0x%lx); this linker links LP64S, the soft-float convention "
                "(-mabi=lp64s)", name,
                (eh->e_flags & EF_LOONGARCH_ABI_MASK) ==
                    EF_LOONGARCH_ABI_DOUBLE_FLOAT ? "double" : "single",
                (unsigned long)eh->e_flags);
        o->machine = eh->e_machine;
        o->eflags = eh->e_flags;
        o->eh = eh;
        o->nsh = eh->e_shnum;
        o->shdrs = (Elf64_Shdr *)(buf + eh->e_shoff);
        if (eh->e_shoff + (Elf64_Off)o->nsh * sizeof(Elf64_Shdr) >
            (Elf64_Off)len)
            die("%s: section headers run past end of file", name);
    }
    o->shstr = (const char *)(buf + o->shdrs[o->eh->e_shstrndx].sh_offset);
    o->sec_out = xmalloc((size_t)o->nsh * sizeof(int));
    o->dbg_sec = xmalloc((size_t)o->nsh * sizeof(int));
    o->dbg_off = xmalloc((size_t)o->nsh * sizeof(long));
    for (int i = 0; i < o->nsh; i++) {
        o->sec_out[i] = -1;
        o->dbg_sec[i] = -1;
        o->dbg_off[i] = 0;
    }

    /* ARM BUILD ATTRIBUTES, checked across inputs.
     *
     * Tag_ABI_VFP_args says where floating-point arguments travel: 0 in
     * the core registers (the base standard, -mfloat-abi=soft), 1 in
     * s0-s15. An object of each kind links without complaint unless
     * somebody compares the tag, and then the callee reads its
     * arguments from registers the caller never wrote -- a
     * miscompilation produced at LINK time, past every check the
     * compiler makes.
     *
     * GNU ld does this comparison. EmbCC does not depend on GNU ld, so
     * EmbCC's linker has to do it, and does: this is the only place in
     * the whole toolchain that can see both objects at once. The same
     * reasoning applies to Tag_ABI_enum_size, where a disagreement
     * changes the layout of every struct holding an enum. */
    for (int i = 0; i < o->nsh; i++) {
        const char *nm = o->shstr + o->shdrs[i].sh_name;
        if (strcmp(nm, ".ARM.attributes") != 0)
            continue;
        arm_attrs_scan(o, buf + o->shdrs[i].sh_offset,
                       (size_t)o->shdrs[i].sh_size);
    }

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

/* ---- debug sections ---------------------------------------------------
 *
 * DWARF is not SHF_ALLOC: it occupies no memory in the running image
 * and has no address. It was therefore dropped entirely, and the
 * linked file carried no .debug_* at all -- so `-g` produced correct
 * objects and an executable no debugger could open. EmbLD wrote its
 * own .embdbg sidecar instead, which embdbg reads and gdb does not.
 *
 * Merging them is simpler here than it is in general, because EmbCC's
 * DWARF writer expresses every cross-reference AS A RELOCATION: a
 * CU's abbrev offset and its stmt_list are absolute relocations
 * against the .debug_abbrev and .debug_line section symbols, and its
 * low_pc/high_pc against .text. So concatenating the sections and
 * resolving those relocations against each object's own contribution
 * rebases everything -- there is no DWARF-aware fixup to write.
 */
static int dbgsec_for(struct linker *l, const char *name)
{
    for (int i = 0; i < l->ndbg; i++)
        if (strcmp(l->dbgsecs[i].name, name) == 0)
            return i;
    if (l->ndbg == l->capdbg) {
        l->capdbg = l->capdbg ? l->capdbg * 2 : 8;
        l->dbgsecs = xrealloc(l->dbgsecs,
                              (size_t)l->capdbg * sizeof *l->dbgsecs);
    }
    {
        struct dbgsec *d = &l->dbgsecs[l->ndbg];
        memset(d, 0, sizeof *d);
        d->name = name;
        return l->ndbg++;
    }
}

static void dbg_append(struct dbgsec *d, const unsigned char *p, long n)
{
    if (d->len + n > d->cap) {
        d->cap = (d->len + n) * 2 + 256;
        d->data = xrealloc(d->data, (size_t)d->cap);
    }
    if (p)
        memcpy(d->data + d->len, p, (size_t)n);
    else
        memset(d->data + d->len, 0, (size_t)n);
    d->len += n;
}

/* MIPS's .MIPS.abiflags: byte 7 is the floating-point ABI (1 double,
 * 2 single, 3 soft, ...; 0 any). Objects that name different ones pass
 * floating-point values in different places and must not be linked
 * together -- the check GNU ld makes, and the one place it can be made. */
static void mips_abiflags_check(struct linker *l, struct object *o,
                                const unsigned char *p, Elf64_Xword n)
{
    int fp;
    if (n < 24)
        die("%s: a .MIPS.abiflags section of %lu bytes; it has 24",
            o->name, (unsigned long)n);
    fp = p[7];
    if (!fp)
        return;
    if (!l->mips_fp_abi) {
        l->mips_fp_abi = fp;
        l->mips_fp_from = o->name;
    } else if (l->mips_fp_abi != fp) {
        die("%s: its floating-point ABI (%s) is not that of %s (%s); "
            "objects compiled for soft float and for an FPU pass floating "
            "point in different registers", o->name,
            fp == 3 ? "soft float" : "an FPU", l->mips_fp_from,
            l->mips_fp_abi == 3 ? "soft float" : "an FPU");
    }
}

/* Collect the object's SHF_ALLOC sections into the global insec list and
 * record where each landed (sec_out), so relocations and symbols can map
 * a (object, section) back to its output placement. */
static void collect_sections(struct linker *l, struct object *o)
{
    for (int i = 0; i < o->nsh; i++) {
        Elf64_Shdr *sh = sh_at(o, i);
        /* MIPS's register-usage and ABI-flag records are ALLOCATED, and
         * nothing in a bare-metal image reads them: checked, then left
         * out (as with no other section, their relocations go nowhere). */
        if (o->machine == EM_MIPS && (sh->sh_type == SHT_MIPS_ABIFLAGS ||
                                      sh->sh_type == SHT_MIPS_REGINFO)) {
            if (sh->sh_type == SHT_MIPS_ABIFLAGS)
                mips_abiflags_check(l, o, o->buf + sh->sh_offset,
                                    sh->sh_size);
            continue;
        }
        if (!(sh->sh_flags & SHF_ALLOC)) {
            /* Not allocated, but DWARF still has to reach the output.
             * Concatenated per name; the offset this object's piece
             * landed at is what its relocations resolve against. */
            const char *nm = o->shstr + sh->sh_name;
            if (l->keep_debug && sh->sh_type == SHT_PROGBITS &&
                strncmp(nm, ".debug_", 7) == 0 && sh->sh_size) {
                int d = dbgsec_for(l, nm);
                o->dbg_sec[i] = d;
                o->dbg_off[i] = l->dbgsecs[d].len;
                dbg_append(&l->dbgsecs[d], o->buf + sh->sh_offset,
                           (long)sh->sh_size);
            }
            continue;
        }
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
        s->sosec = -1;
        s->shflags = sh->sh_flags;
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
        /* .rodata rides in the TEXT segment on every von Neumann target,
         * because the processor can read it where it lies.
         *
         * AVR cannot. Program space and data space are separate address
         * spaces there, and `ld`/`lds` reach only the data one -- a string
         * literal left in flash is not slow to read, it is UNREADABLE by
         * any instruction the compiler emits for `*s`. So on a Harvard
         * target read-only data joins the writable segment, gets a RAM
         * address and a flash load address like .data, and the startup
         * copies it across with `lpm`. That is what avr-gcc's linker
         * script does and why `const char *s = "hi"` costs RAM there. */
        int harvard_ro = o->machine == EM_AVR && s->osec == OSEC_RODATA;
        s->seg = (!harvard_ro &&
                  (s->osec == OSEC_VECTORS ||
                   s->osec == OSEC_TEXT || s->osec == OSEC_RODATA ||
                   (s->osec >= OSEC_COUNT &&
                    !l->orphans[s->osec - OSEC_COUNT].writable)))
                     ? SEG_TEXT : SEG_DATA;
        o->sec_out[i] = l->nsec;
        l->nsec++;
    }
}

/* The TriCore entry stub, a fixed TC_STUB_SIZE bytes (every address is a
 * MOVH/ADDI or MOVH.A/LEA pair, both halves always, so the size is known
 * before layout):
 *
 *     A10 = the stack top
 *     link each CSA in [csa_start, csa_end) to the next, the last to none;
 *     FCX = the first, LCX = the one 16 before the end (the depletion
 *     trap comes with room left to handle it)
 *     PCXI = 0, PSW.CDC = 0x7f (call depth counting off), isync
 *     JI to the entry
 *
 * A CSA link word holds the address's segment (bits 31:28) in bits 19:16
 * and address bits 21:6 in 15:0, so consecutive 64-byte areas have
 * consecutive links. Without the list, the entry's first CALL traps. */
#define TC_STUB_SIZE 116

static unsigned long tc_csa_link(unsigned long a)
{
    return ((a >> 12) & 0xf0000UL) | ((a >> 6) & 0xffffUL);
}

static void tc_li_fixed(struct code *c, int d, unsigned long v)
{
    tc_movh(c, d, tc_hi_adj(v));
    tc_addi(c, d, d, (long long)(short)(unsigned short)(v & 0xffff));
}

static void tc_li_a_fixed(struct code *c, int a, unsigned long v)
{
    tc_movh_a(c, a, tc_hi_adj(v));
    tc_lea(c, a, a, (long long)(short)(unsigned short)(v & 0xffff));
}

static void fill_tricore_stub(struct linker *l, struct code *c,
                              Elf64_Addr entry)
{
    unsigned long n = (l->csa_end - l->csa_start) / 64;
    unsigned long last = n > 32 ? n - 16 : n - 2;
    int top;
    tc_li_a_fixed(c, TC_SP, (unsigned long)l->stack_top);
    tc_li_a_fixed(c, 12, l->csa_start);
    tc_li_fixed(c, 0, tc_csa_link(l->csa_start));
    tc_mov(c, 2, 0);                               /* FCX: the first */
    tc_li_fixed(c, 1, n - 1);
    top = c->len;
    tc_addi(c, 0, 0, 1);                           /* the next one's link */
    tc_store(c, 0, 12, 0, 4);
    tc_lea(c, 12, 12, 64);
    tc_addi(c, 1, 1, -1);
    tc_w(c, tc_enc_jcci(TC_JNE, 1, 0, (long)top - (long)c->len));
    tc_mov_imm(c, 0, 0);
    tc_store(c, 0, 12, 0, 4);                      /* the last: no next */
    tc_mtcr(c, TC_CSFR_FCX, 2);
    tc_li_fixed(c, 0, tc_csa_link(l->csa_start + 64 * last));
    tc_mtcr(c, TC_CSFR_LCX, 0);
    tc_mov_imm(c, 0, 0);
    tc_mtcr(c, TC_CSFR_PCXI, 0);
    tc_mfcr(c, 0, TC_CSFR_PSW);
    tc_alu_imm(c, TC_OR, 0, 0, 0x7f);
    tc_mtcr(c, TC_CSFR_PSW, 0);
    tc_isync(c);
    tc_li_a_fixed(c, 12, (unsigned long)entry);
    tc_ji(c, 12);
    if (c->len != TC_STUB_SIZE)
        internal_error("the TriCore entry stub is %d bytes, not %d", c->len,
                       TC_STUB_SIZE);
}

/* The RISC-V entry stub: set sp, then jump to the real entry.
 *
 * Registered as an ordinary input section in the .vectors group, which
 * already sorts AHEAD of .text (it is where a Cortex-M's vector table
 * goes), so the stub lands at the image base and the bytes travel
 * through the same path as everything else. The instructions are filled
 * in after layout, when the entry's address exists.
 *
 * The jump is `auipc`+`jalr` rather than a bare `jal`, which reaches 1MB
 * where this reaches 2GB -- and a fixed eight bytes means the layout does
 * not depend on how far the entry turns out to be. The sp materialisation
 * in front of it is however long rv_li needs for THAT address, which is
 * known before layout because it is a constant from the command line. */
static void add_entry_stub(struct linker *l)
{
    int xlen = l->elf32 ? 32 : 64;
    /* MIPS: li sp, then lui/ori t9 with the entry's absolute address and
     * a jr through it, its delay slot a nop -- the lui/ori pair always
     * both, so the size is known before layout. */
    /* Xtensa: `j` over three literals (sp, PS, the entry), then sp and
     * its bottom frame, PS for the windowed ABI, and a callx8 to the
     * entry -- which is then an ordinary windowed function -- and a loop
     * should it return: 4 + 12 + 12 * 3 bytes. */
    /* ARM (an A-profile core, which comes out of reset in ARM state
     * with sp unset): movw/movt sp, movw/movt ip with the entry, bx ip --
     * five A32 words, whatever the addresses, and bx interworks if the
     * entry is Thumb. */
    long size = l->machine == EM_MIPS
              ? mips_li_len((long long)l->stack_top) + 16
              : l->machine == EM_LOONGARCH
              ? la_li_len((long long)l->stack_top) + 8
              : l->machine == EM_ARM ? 20
              : l->machine == EM_TRICORE ? TC_STUB_SIZE
              : l->machine == EM_XTENSA ? 52
              : l->machine == EM_PPC ? 32
              : l->machine == EM_RX ? 14
              : l->machine == EM_SPARC ? 20
              : l->machine == EM_68K ? 12
              : rv_li_len((long long)l->stack_top, xlen) + 8;

    if (l->nsec == l->capsec) {
        l->capsec = l->capsec ? l->capsec * 2 : 64;
        l->insecs = xrealloc(l->insecs,
                             (size_t)l->capsec * sizeof *l->insecs);
    }
    l->stub = xcalloc((size_t)size, 1);
    l->stub_size = size;
    struct insec *s = &l->insecs[l->nsec];
    memset(s, 0, sizeof *s);
    s->obj = NULL;              /* synthetic: apply_relocs never sees it */
    s->shndx = -1;
    s->name = ".start";
    s->size = (Elf64_Xword)size;
    s->align = 4;
    s->data = l->stub;
    s->osec = OSEC_VECTORS;
    s->seg = SEG_TEXT;
    l->stub_sec = l->nsec;
    l->nsec++;
}

static void fill_entry_stub(struct linker *l, Elf64_Addr entry)
{
    struct code c = { l->stub, 0, (int)l->stub_size, NULL, 0, 0 };
    struct insec *s = &l->insecs[l->stub_sec];
    int xlen = l->elf32 ? 32 : 64;
    if (l->machine == EM_ARM) {
        a32_movw_movt(&c, 13, (unsigned)(l->stack_top & 0xffff), 0);
        a32_movw_movt(&c, 13, (unsigned)((l->stack_top >> 16) & 0xffff), 1);
        a32_movw_movt(&c, 12, (unsigned)(entry & 0xffff), 0);
        a32_movw_movt(&c, 12, (unsigned)((entry >> 16) & 0xffff), 1);
        a32_bx(&c, 12, 0);
        return;
    }
    if (l->machine == EM_TRICORE) {
        fill_tricore_stub(l, &c, entry);
        return;
    }
    if (l->machine == EM_XTENSA) {
        /*   0  j     16          (over the literals)
         *   4  .word stack_top - 32, PS, entry
         *  16  l32r  a1, 4       sp, below a 32-byte bottom frame
         *      addi  a2, a1, 32
         *      addi  a3, a1, -16
         *      s32i  a2, a3, 4   [sp - 12] = stack_top, see below
         *      l32r  a2, 8       PS: WOE (bit 18) | UM (bit 5), level 0
         *      wsr   a2, ps
         *      movi  a2, 1
         *      wsr   a2, windowstart   this window, the only live one
         *      rsync
         *      l32r  a8, 12
         *      callx8 a8
         *      j     .
         *
         * The stub's own window is the bottom of the call stack, and when
         * the calls nest deep enough the window overflow handler spills
         * it like any other: its a0-a3 below its callee's sp, and its
         * a4-a7 below ITS caller's sp, which the handler reads from the
         * stub's [sp - 12] -- where a real caller's spill would have
         * left it. Nothing has, so the stub puts stack_top there, and the
         * 32 bytes it sits under are the room for those four words.
         * WINDOWSTART says which windows hold a live frame, and a core
         * started anywhere but its reset vector may hold anything there
         * (QEMU's loader leaves 0): it is set to this window alone. */
        xt_w(&c, xt_enc_j(16 - 4));
        code_byte(&c, 0);
        code_u32(&c, ((unsigned long)l->stack_top - 32) & 0xffffffffUL);
        code_u32(&c, (1UL << 18) | (1UL << 5));
        code_u32(&c, (unsigned long)entry & 0xffffffffUL);
        xt_w(&c, xt_enc_l32r(XT_A1, c.len, 4));
        xt_addi(&c, XT_A2, XT_A1, 32);
        xt_addi(&c, XT_A3, XT_A1, -16);
        xt_store(&c, XT_A2, XT_A3, 4, 4);
        xt_w(&c, xt_enc_l32r(XT_A2, c.len, 8));
        xt_wsr(&c, XT_A2, XT_SR_PS);
        xt_movi(&c, XT_A2, 1);
        xt_wsr(&c, XT_A2, XT_SR_WINDOWSTART);
        xt_rsync(&c);
        xt_w(&c, xt_enc_l32r(XT_A8, c.len, 12));
        xt_callx(&c, 2, XT_A8);
        xt_w(&c, xt_enc_j(-4));
        (void)s;
        return;
    }
    if (l->machine == EM_PPC) {
        /* r1 = the stack's top, then the first frame below it with a
         * zero back chain (the end of the chain), and a bctr to the
         * entry -- lis/ori always both, so the size is known before
         * layout. */
        ppc_lis(&c, PPC_SP, (unsigned)(l->stack_top >> 16) & 0xffff);
        ppc_imm(&c, PPC_ORI, PPC_SP, PPC_SP,
                (long long)(l->stack_top & 0xffff));
        ppc_li(&c, PPC_R0, 0);
        ppc_stwu(&c, PPC_R0, PPC_SP, -16);
        ppc_lis(&c, PPC_R12, (unsigned)(entry >> 16) & 0xffff);
        ppc_imm(&c, PPC_ORI, PPC_R12, PPC_R12, (long long)(entry & 0xffff));
        ppc_mtctr(&c, PPC_R12);
        ppc_bctr(&c);
        return;
    }
    if (l->machine == EM_RX) {
        /* mov.l #top, r0; mov.l #entry, r14; jmp r14 -- both moves in
         * their six-byte form, so the size is known before layout */
        rx_mov_abs(&c, RX_SP, (unsigned long)l->stack_top);
        rx_mov_abs(&c, RX_R14, (unsigned long)entry);
        rx_jmp(&c, RX_R14);
        return;
    }
    if (l->machine == EM_SPARC) {
        /* %sp = the stack's top less a 96-byte frame (the window save
         * area the first function's save writes below it is its own),
         * then sethi/jmp to the entry, its delay slot a nop -- always five
         * words, so the size is known before layout. */
        unsigned long sp = (unsigned long)(l->stack_top - 96) & 0xffffffffUL;
        unsigned long e = (unsigned long)entry & 0xffffffffUL;
        sparc_sethi(&c, SP_SP, sp >> 10);
        sparc_alu_imm(&c, SP_OR, SP_SP, SP_SP, (long long)(sp & 0x3ff));
        sparc_sethi(&c, SP_G1, e >> 10);
        sparc_jmpl(&c, SP_G0, SP_G1, (long long)(e & 0x3ff));
        sparc_nop(&c);
        return;
    }
    if (l->machine == EM_68K) {
        /* movea.l #top,%sp; jmp entry (abs.l): the encodings
         * tests/golden/coldfire-encoding.sh referees (cf_move, cf_jmp) */
        unsigned long top = (unsigned long)l->stack_top & 0xffffffffUL;
        unsigned long e = (unsigned long)entry & 0xffffffffUL;
        unsigned char *p = l->stub;
        (void)c;
        p[0] = 0x2e; p[1] = 0x7c;
        p[2] = (unsigned char)(top >> 24); p[3] = (unsigned char)(top >> 16);
        p[4] = (unsigned char)(top >> 8);  p[5] = (unsigned char)top;
        p[6] = 0x4e; p[7] = 0xf9;
        p[8] = (unsigned char)(e >> 24);   p[9] = (unsigned char)(e >> 16);
        p[10] = (unsigned char)(e >> 8);   p[11] = (unsigned char)e;
        return;
    }
    if (l->machine == EM_MIPS) {
        /* MIPS64 too, where lui and addiu sign-extend: both addresses
         * must be the sign extension of their low 32 bits (KSEG0/1,
         * 0xffffffff80000000 up), as a MIPS64 board's RAM is reached */
        if (!l->elf32 &&
            ((long long)l->stack_top != (long long)(int)l->stack_top ||
             (long long)entry != (long long)(int)entry))
            die("-Tstack on MIPS64: the stack top 0x%llx and the entry "
                "0x%llx must be sign-extended 32-bit addresses (KSEG0 is "
                "0xffffffff80000000)", (unsigned long long)l->stack_top,
                (unsigned long long)entry);
        mips_li(&c, MIPS_SP, (long long)l->stack_top);
        mips_lui(&c, MIPS_T9, (unsigned)(entry >> 16) & 0xffff);
        mips_alu_imm(&c, MIPS_ORI, MIPS_T9, MIPS_T9, (long long)(entry & 0xffff));
        mips_jr(&c, MIPS_T9);
        mips_nop(&c);
        return;
    }
    if (l->machine == EM_LOONGARCH) {
        /* li sp, then pcaddu18i t0 + jirl zero, t0 to the entry: +-128
         * GiB, and a fixed eight bytes whatever the distance */
        Elf64_Addr pc_at = s->vaddr + (Elf64_Addr)l->stub_size - 8;
        long long dd = (long long)entry - (long long)pc_at, hi;
        la_li(&c, LA_SP, (long long)l->stack_top);
        hi = (dd + 0x20000) >> 18;
        la_pcrel(&c, LA_PCADDU18I, LA_T0, (long)hi);
        la_jirl(&c, LA_ZERO, LA_T0, (long)(dd - (hi << 18)));
        return;
    }
    /* The auipc sits just before the jalr, at the end of the stub. */
    Elf64_Addr auipc_at = s->vaddr + (Elf64_Addr)l->stub_size - 8;
    long long d = (long long)entry - (long long)auipc_at;

    if (d < -(1LL << 31) || d >= (1LL << 31))
        die("the entry symbol is more than 2GB from the image base");
    /* rv_li and not a hand-rolled lui/addi: at RV64 `lui` sign-extends
     * bit 31, so the pair cannot produce 0x80800000 -- it produces
     * 0xffffffff80800000, and the first push faulted on an address that
     * looked almost right. rv_li knows to build the wider value. */
    rv_li(&c, RV_SP, (long long)l->stack_top, xlen);
    rv_auipc(&c, RV_T0, (long)(((d + 0x800) >> 12) & 0xfffff));
    /* jalr with rd = zero is a tail jump: nothing returns to the stub. */
    rv_jalr(&c, RV_ZERO, RV_T0, (int)(((d & 0xfff) ^ 0x800) - 0x800));
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
        l->harvard = o->machine == EM_AVR;
        l->elf32 = o->elf32;
        l->big_endian = o->big_endian;
        /* the MIPS words this link reads, patches and writes (the entry
         * stub) are in the objects' order */
        mips_set_big_endian(o->big_endian);
    } else if (o->machine != l->machine) {
        die("%s: an object for a different machine than the ones before "
            "it (%u against %u)", o->name, (unsigned)o->machine,
            (unsigned)l->machine);
    } else if (o->big_endian != l->big_endian) {
        die("%s: a %s-endian object, and the ones before it are %s-endian; "
            "an image has one byte order", o->name,
            o->big_endian ? "big" : "little",
            l->big_endian ? "big" : "little");
    }
    if (o->machine == EM_RISCV) {
        /* The float ABI is every object's or none's: an ilp32f caller
         * leaves a float argument in fa0 and an ilp32 callee reads a0. The
         * first object decides, as for the machine. */
        unsigned long fa = o->eflags & EF_RISCV_FLOAT_ABI_MASK;
        static const char *const fname[] = { "soft-float (ilp32/lp64)",
            "single-float (ilp32f/lp64f)", "double-float (ilp32d/lp64d)",
            "quad-float" };
        if (l->nobj && fa != (l->eflags & EF_RISCV_FLOAT_ABI_MASK))
            die("%s: a %s object, and the ones before it are %s: they "
                "disagree about which registers carry floating-point "
                "arguments, so every call between them would read one the "
                "caller never wrote. Build both with the same -mabi=",
                o->name, fname[fa >> 1],
                fname[(l->eflags & EF_RISCV_FLOAT_ABI_MASK) >> 1]);
        l->eflags |= (o->eflags & EF_RISCV_RVC) | fa;
    } else if (o->machine == EM_MIPS)
        /* the architecture and the ABI, from the first object; NOREORDER
         * is a property of each object's code and not of the image */
        l->eflags = l->eflags ? l->eflags
                              : o->eflags & (EF_MIPS_ARCH_MASK | 0xf000UL);
    else if (o->machine == EM_RX)
        l->eflags = l->eflags ? l->eflags : o->eflags;
    else if (o->machine == EM_AVR && !(l->eflags & EF_AVR_ARCH_MASK))
        l->eflags = o->eflags & EF_AVR_ARCH_MASK;
    else if (o->machine == EM_LOONGARCH)
        l->eflags = EF_LOONGARCH_ABI_SOFT_FLOAT | EF_LOONGARCH_OBJABI_V1;
    else if (o->machine == EM_TRICORE && !(l->eflags & EF_TRICORE_CORE_MASK))
        l->eflags = o->eflags & EF_TRICORE_CORE_MASK;
    else if (o->machine == EM_68K)
        /* the image needs the highest ColdFire ISA any object does
         * (binutils numbers them in order: A, A+, B, C) */
        l->eflags = (o->eflags & 0x0fUL) > (l->eflags & 0x0fUL)
                  ? o->eflags : l->eflags;
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
static struct symbol *defines_needed(struct linker *l, struct object *o)
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
            return g;
    }
    return NULL;
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
                struct symbol *need = defines_needed(l, mem->obj);
                if (!need)
                    continue;
                mem->obj->pulled_for = need->name;
                mem->obj->pulled_by = need->ref;
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

        if (sy->st_shndx == SHN_UNDEF) {
            if (!g->ref)
                g->ref = o;
            continue; /* a reference; may be satisfied by a later object */
        }

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
        /* Two weak definitions: the first one the link meets stands, as
         * GNU ld and lld have it -- a harness's weak write() given ahead
         * of lib/libc was replaced by libc's own weak default when
         * __os_write pulled that member in, and stdout went nowhere. */
        if (g->defined && !g->common && g->weak && weak && g->obj)
            continue;

        g->defined = 1;
        g->common = 0;
        g->weak = weak;
        g->obj = o;
        g->value = sy->st_value;
        /* The SIZE travels too, and not only for COMMON as it used to:
         * a debugger filters the symbol table on STT_FUNC with a nonzero
         * size, so a linked image whose functions all had size 0 was one
         * EmbDBG could see no functions in. */
        g->size = sy->st_size;
        /* And the TYPE, from the input rather than inferred from which
         * segment it landed in: a `const void *vectors[]` in .vectors is
         * in the text segment and is not a function, and a debugger that
         * is told it is will try to disassemble a table of addresses. */
        g->type = ELF64_ST_TYPE(sy->st_info);
        g->insec = (sy->st_shndx == SHN_ABS) ? -1
                                             : o->sec_out[sy->st_shndx];
    }
}

/* Whether a symbol goes in the image's symbol table: defined, named,
 * and not in a section the link removed (/DISCARD/ or --gc-sections),
 * which has no address -- written out, it would name a place where
 * other code now lies. */
static int sym_in_image(const struct linker *l, const struct symbol *g)
{
    return g->defined && g->name && *g->name &&
           !(g->insec >= 0 && !g->scripted && l->insecs[g->insec].discarded);
}

/* ---- --gc-sections ----------------------------------------------------
 *
 * Every allocated input section that nothing kept refers to is dropped,
 * as GNU ld does it: mark from the roots, follow each marked section's
 * relocations to the sections their symbols are in, and discard what the
 * mark never reached. With -ffunction-sections and -fdata-sections each
 * function and object is a section of its own, so what goes is exactly
 * the code and data the program cannot reach.
 *
 * The roots: the entry symbol, every -u and EXTERN name, what a script
 * KEEP()s, and the sections a program is entitled to have kept without
 * naming them -- the vector table (without a script; with one, it is
 * KEEP's job, as in every vendor script), the constructor and destructor
 * arrays and .init/.fini, notes, a section marked SHF_GNU_RETAIN, and any
 * section whose __start_/__stop_ bounds the program refers to. A section
 * with SHF_LINK_ORDER (ARM's .ARM.exidx) lives exactly when the section
 * it describes does. Non-allocated sections (DWARF) are never collected;
 * their references to a collected function resolve to 0, as ld's do. */

#define SHF_GNU_RETAIN_ 0x200000u

/* .eh_frame is a root, but an FDE's pc_begin is not a reference: every
 * function has one, and following them would keep everything. So the
 * record a relocation lands in is looked up -- an FDE's pc_begin field,
 * 8 bytes in, is skipped -- and after the sweep an FDE whose function
 * went gets a pc_range of 0 (eh_neutralize), which no pc falls in. */
static int eh_is_fde_pc(const unsigned char *p, Elf64_Xword size,
                        Elf64_Addr off)
{
    Elf64_Xword at = 0;
    while (at + 8 <= size) {
        unsigned len = (unsigned)p[at] | (unsigned)p[at + 1] << 8 |
                       (unsigned)p[at + 2] << 16 | (unsigned)p[at + 3] << 24;
        if (len == 0 || len == 0xffffffffu)
            return 0;               /* the terminator, or 64-bit DWARF */
        if (off < at + 4 + len) {
            unsigned id = (unsigned)p[at + 4] | (unsigned)p[at + 5] << 8 |
                          (unsigned)p[at + 6] << 16 |
                          (unsigned)p[at + 7] << 24;
            return id != 0 && off == at + 8;
        }
        at += 4 + len;
    }
    return 0;
}

static unsigned long eh_uleb(const unsigned char **q, const unsigned char *e)
{
    unsigned long v = 0;
    int sh = 0;
    while (*q < e) {
        unsigned char b = *(*q)++;
        if (sh < 64)
            v |= (unsigned long)(b & 0x7f) << sh;
        sh += 7;
        if (!(b & 0x80))
            break;
    }
    return v;
}

/* The size of a value in pointer encoding `enc`, 0 if unknown. */
static int eh_enc_size(int enc, int addr)
{
    switch (enc & 0x0f) {
    case 0x00: return addr;                 /* absptr */
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    case 0x04: case 0x0c: return 8;
    default:   return 0;                    /* uleb/sleb: not in an FDE */
    }
}

/* The FDE pointer encoding of the CIE at p (its length field), or -1. */
static int eh_cie_fde_enc(const unsigned char *p, const unsigned char *end,
                          int addr)
{
    const unsigned char *q = p + 8;
    if (q >= end)
        return -1;
    int version = *q++;
    const char *aug = (const char *)q;
    while (q < end && *q)
        q++;
    if (q >= end)
        return -1;
    q++;
    if (aug[0] && aug[0] != 'z')
        return aug[0] == 0 ? 0 : -1;
    eh_uleb(&q, end);                       /* code alignment */
    eh_uleb(&q, end);                       /* data alignment (sleb) */
    if (version == 1) q++; else eh_uleb(&q, end);   /* return register */
    if (!aug[0])
        return 0;                           /* absptr */
    eh_uleb(&q, end);                       /* augmentation length */
    for (const char *a = aug + 1; *a && q < end; a++) {
        if (*a == 'R')
            return *q;
        if (*a == 'L')
            q++;
        else if (*a == 'P') {
            int pe = *q++, n = eh_enc_size(pe, addr);
            if (!n)
                return -1;
            q += n;
        } else if (*a != 'S' && *a != 'B')
            return -1;
    }
    return 0;
}

static int gc_target_insec(struct linker *l, struct object *o,
                           Elf64_Word symi);

static void eh_neutralize(struct linker *l, struct insec *s)
{
    struct object *o = s->obj;
    int rs = o->relsec[s->shndx];
    if (rs < 0)
        return;
    unsigned char *p = o->buf + sh_at(o, s->shndx)->sh_offset;
    const unsigned char *end = p + s->size;
    Elf64_Shdr *rsh = sh_at(o, rs);
    int isrel = rsh->sh_type == SHT_REL;
    int relsz = o->elf32 ? (isrel ? (int)sizeof(Elf32_Rel)
                                  : (int)sizeof(Elf32_Rela))
                         : (isrel ? (int)sizeof(Elf64_Rel)
                                  : (int)sizeof(Elf64_Rela));
    const unsigned char *rb = o->buf + rsh->sh_offset;
    int n = (int)(rsh->sh_size / (Elf64_Xword)relsz), addr = o->elf32 ? 4 : 8;
    for (int j = 0; j < n; j++) {
        Elf64_Addr off;
        Elf64_Word symi;
        if (o->elf32) {
            const Elf32_Rel *r = (const Elf32_Rel *)(rb + (long)j * relsz);
            off = r->r_offset;
            symi = r->r_info >> 8;
        } else {
            const Elf64_Rel *r = (const Elf64_Rel *)(rb + (long)j * relsz);
            off = r->r_offset;
            symi = (Elf64_Word)ELF64_R_SYM(r->r_info);
        }
        if (!eh_is_fde_pc(p, s->size, off))
            continue;
        int t = gc_target_insec(l, o, symi);
        if (t < 0 || !l->insecs[t].gc)
            continue;
        /* the CIE: the backward pointer at the FDE's offset 4 */
        const unsigned char *fid = p + off - 4;
        unsigned back = (unsigned)fid[0] | (unsigned)fid[1] << 8 |
                        (unsigned)fid[2] << 16 | (unsigned)fid[3] << 24;
        if ((Elf64_Addr)back > off - 4)
            die("%s: .eh_frame: an FDE whose CIE is outside the section",
                o->name);
        int enc = eh_cie_fde_enc(p + (off - 4 - back), end, addr);
        int sz = enc < 0 ? 0 : eh_enc_size(enc, addr);
        if (!sz || off + 2 * (Elf64_Addr)sz > s->size)
            die("%s: .eh_frame: an FDE for a function --gc-sections "
                "removed, in an encoding this linker cannot read", o->name);
        memset(p + off + sz, 0, (size_t)sz);       /* pc_range = 0 */
    }
}

static void gc_mark(struct linker *l, int si, int *work, int *nw)
{
    if (si < 0 || l->insecs[si].live)
        return;
    l->insecs[si].live = 1;
    work[(*nw)++] = si;
}

static void gc_mark_sym(struct linker *l, const char *name, int *work,
                        int *nw)
{
    struct symbol *g = sym_find(l, name);
    if (g && g->defined && !g->scripted && !g->common)
        gc_mark(l, g->insec, work, nw);
}

static int gc_root_name(const char *n)
{
    static const char *const pfx[] = {
        ".init_array", ".fini_array", ".preinit_array", ".ctors", ".dtors",
        ".init", ".fini", ".jcr",
    };
    for (size_t i = 0; i < sizeof pfx / sizeof pfx[0]; i++) {
        size_t k = strlen(pfx[i]);
        if (!strncmp(n, pfx[i], k) && (n[k] == 0 || n[k] == '.'))
            return 1;
    }
    return 0;
}

/* The input section relocation symbol symi of o is in, or -1. */
static int gc_target_insec(struct linker *l, struct object *o,
                           Elf64_Word symi)
{
    if (!symi || symi >= (Elf64_Word)o->nsym)
        return -1;
    Elf64_Sym *sy = &o->syms[symi];
    if (ELF64_ST_BIND(sy->st_info) == STB_LOCAL) {
        if (sy->st_shndx != SHN_UNDEF && sy->st_shndx < (unsigned)o->nsh)
            return o->sec_out[sy->st_shndx];
        return -1;
    }
    const char *nm = o->symstr + sy->st_name;
    struct symbol *g = *nm ? sym_find(l, nm) : NULL;
    if (g && g->defined && !g->scripted && !g->common)
        return g->insec;
    return -1;
}

static void gc_sections(struct linker *l, const char *const *undefs,
                        int nundefs)
{
    int *work = xmalloc((size_t)(l->nsec ? l->nsec : 1) * sizeof *work);
    int nw = 0;
    /* each object's relocation section for each of its sections */
    for (int i = 0; i < l->nobj; i++) {
        struct object *o = l->objs[i];
        o->relsec = xmalloc((size_t)(o->nsh ? o->nsh : 1) * sizeof *o->relsec);
        for (int k = 0; k < o->nsh; k++)
            o->relsec[k] = -1;
        for (int k = 0; k < o->nsh; k++) {
            Elf64_Shdr *sh = sh_at(o, k);
            if ((sh->sh_type == SHT_REL || sh->sh_type == SHT_RELA) &&
                sh->sh_info < (Elf64_Word)o->nsh)
                o->relsec[sh->sh_info] = k;
        }
    }

    gc_mark_sym(l, l->entry, work, &nw);
    for (int k = 0; k < nundefs; k++)
        gc_mark_sym(l, undefs[k], work, &nw);
    if (l->sc)
        for (int k = 0; k < l->sc->nextern; k++)
            gc_mark_sym(l, l->sc->externs[k], work, &nw);
    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        Elf64_Shdr *sh = s->obj ? sh_at(s->obj, s->shndx) : NULL;
        if (!s->obj || s->keep || (!l->sc && s->osec == OSEC_VECTORS) ||
            gc_root_name(s->name) || !strcmp(s->name, ".eh_frame") ||
            (sh && (sh->sh_type == SHT_INIT_ARRAY ||
                    sh->sh_type == SHT_FINI_ARRAY ||
                    sh->sh_type == SHT_PREINIT_ARRAY ||
                    sh->sh_type == SHT_NOTE ||
                    (sh->sh_flags & SHF_GNU_RETAIN_))))
            gc_mark(l, i, work, &nw);
    }
    /* __start_NAME / __stop_NAME referred to: every NAME section */
    for (int i = 0; i < l->nsym; i++) {
        const char *n = l->syms[i].name, *sec = NULL;
        if (!strncmp(n, "__start_", 8)) sec = n + 8;
        else if (!strncmp(n, "__stop_", 7)) sec = n + 7;
        if (!sec || !*sec)
            continue;
        for (int k = 0; k < l->nsec; k++)
            if (!strcmp(l->insecs[k].name, sec))
                gc_mark(l, k, work, &nw);
    }

    for (;;) {
        while (nw) {
            struct insec *s = &l->insecs[work[--nw]];
            struct object *o = s->obj;
            int rs = o ? o->relsec[s->shndx] : -1;
            if (rs < 0)
                continue;
            Elf64_Shdr *rsh = sh_at(o, rs);
            int isrel = rsh->sh_type == SHT_REL;
            int relsz = o->elf32 ? (isrel ? (int)sizeof(Elf32_Rel)
                                          : (int)sizeof(Elf32_Rela))
                                 : (isrel ? (int)sizeof(Elf64_Rel)
                                          : (int)sizeof(Elf64_Rela));
            const unsigned char *rb = o->buf + rsh->sh_offset;
            int n = (int)(rsh->sh_size / (Elf64_Xword)relsz);
            int eh = !strcmp(s->name, ".eh_frame") && s->data;
            for (int j = 0; j < n; j++) {
                Elf64_Word symi;
                Elf64_Addr off;
                if (o->elf32) {
                    const Elf32_Rel *r = (const Elf32_Rel *)(rb + (long)j * relsz);
                    symi = r->r_info >> 8;
                    off = r->r_offset;
                } else {
                    const Elf64_Rel *r = (const Elf64_Rel *)(rb + (long)j * relsz);
                    symi = (Elf64_Word)ELF64_R_SYM(r->r_info);
                    off = r->r_offset;
                }
                if (eh && eh_is_fde_pc(s->data, s->size, off))
                    continue;
                gc_mark(l, gc_target_insec(l, o, symi), work, &nw);
            }
        }
        /* SHF_LINK_ORDER: kept with the section it is about */
        for (int i = 0; i < l->nsec; i++) {
            struct insec *s = &l->insecs[i];
            if (s->live || !s->obj || !(s->shflags & SHF_LINK_ORDER))
                continue;
            Elf64_Word ln = sh_at(s->obj, s->shndx)->sh_link;
            if (ln < (Elf64_Word)s->obj->nsh && s->obj->sec_out[ln] >= 0 &&
                l->insecs[s->obj->sec_out[ln]].live)
                gc_mark(l, i, work, &nw);
        }
        if (!nw)
            break;
    }

    for (int i = 0; i < l->nsec; i++) {
        struct insec *s = &l->insecs[i];
        if (s->live || s->discarded)
            continue;
        s->discarded = s->gc = 1;
        if (l->print_gc)
            fprintf(stderr, "embld: removing unused section '%s' in file "
                    "'%s'\n", s->name, s->obj ? s->obj->name : "?");
    }
    for (int i = 0; i < l->nsec; i++)
        if (l->insecs[i].live && l->insecs[i].obj &&
            !strcmp(l->insecs[i].name, ".eh_frame") && l->insecs[i].data)
            eh_neutralize(l, &l->insecs[i]);
    free(work);
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
static unsigned long ls_init_priority(const char *name);

/* The members of group `os` in placement order. Input order, except in
 * .init_array and .fini_array, which GNU ld's default script lays out
 * SORT_BY_INIT_PRIORITY(.init_array.*) first, then the plain arrays: a
 * constructor(101)'s .init_array.00101 runs before a constructor(200)'s,
 * and both before every unnumbered one (the startup walks the array in
 * order, the exit walks .fini_array backwards). Stable: a priority's
 * inputs stay in input order. Returns how many, in *out (caller frees). */
static int osec_members(struct linker *l, int os, int **out)
{
    int n = 0, *v = xmalloc((size_t)(l->nsec + 1) * sizeof *v);
    for (int i = 0; i < l->nsec; i++)
        if (l->insecs[i].osec == os && !l->insecs[i].discarded)
            v[n++] = i;
    if (os == OSEC_INIT_ARRAY || os == OSEC_FINI_ARRAY)
        for (int i = 1; i < n; i++) {           /* insertion: stable */
            int x = v[i], j = i - 1;
            unsigned long px = ls_init_priority(l->insecs[x].name);
            while (j >= 0 && ls_init_priority(l->insecs[v[j]].name) > px) {
                v[j + 1] = v[j];
                j--;
            }
            v[j + 1] = x;
        }
    *out = v;
    return n;
}

static void place_osec(struct linker *l, int os, Elf64_Addr *va,
                       struct osec_bound *b)
{
    int *m, n = osec_members(l, os, &m);
    /* the group starts where its first member does — the padding before
     * that member is not part of the group, or a bracket-walked table
     * would begin with it */
    if (n)
        *va = align_up(*va, l->insecs[m[0]].align);
    b[os].start = *va;
    for (int k = 0; k < n; k++) {
        struct insec *s = &l->insecs[m[k]];
        *va = align_up(*va, s->align);
        s->vaddr = *va;
        *va += s->size;
    }
    b[os].end = *va;
    free(m);
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
    /* On a Harvard target .rodata is placed with the writable segment
     * below, not here: see `harvard` in struct linker. Placing it in both
     * would double-count the location counter; placing it in neither
     * would leave it unaddressed and silently dropped, which is exactly
     * what happened when only the per-section `seg` was changed and this
     * order was not. */
    if (!l->harvard)
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
        /* Stored WHERE IT IS ADDRESSED, which is the ordinary case and
         * has to be said rather than left at zero: __data_load is a real
         * symbol in every link, and a startup that copies .data from it
         * -- which is the same startup a firmware build uses -- read
         * from address 0 and hung. Equal brackets make that copy a
         * correct no-op instead. */
        l->data_lma = va;
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
             l->insecs[i].osec == OSEC_TBSS) && !l->insecs[i].discarded &&
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

    /* First in the writable segment on a Harvard target, so that the
     * string literals a program reads sit at the bottom of its RAM image
     * and the .data that follows keeps the order every other target has. */
    if (l->harvard)
        place_osec(l, OSEC_RODATA, &va, b);
    place_osec(l, OSEC_INIT_ARRAY, &va, b);
    place_osec(l, OSEC_FINI_ARRAY, &va, b);
    place_osec(l, OSEC_CTORS, &va, b);
    place_osec(l, OSEC_DTORS, &va, b);
    place_osec(l, OSEC_DATA, &va, b);
    for (int i = 0; i < l->norphan; i++)
        if (l->orphans[i].writable)
            place_osec(l, OSEC_COUNT + i, &va, b);
    /* MIPS: a word store to an address that is not a multiple of four
     * traps, and a startup's word loops (define_firmware_symbols) run
     * from __data_end and __bss_start to __bss_end. A .data that ended
     * two bytes into a word sent the harness's .bss loop to the reset
     * vector. Both ends are kept on word boundaries, the padding inside
     * the image, as a GNU script's ALIGN(4) puts it. ARM and RISC-V too:
     * ARMv6-M and ARMv8-M Baseline have no unaligned access, and a
     * Cortex-M0 image with 37 bytes of .data took a HardFault in its
     * .bss loop before main; a RISC-V core may trap the same store. */
    if (l->machine == EM_MIPS || l->machine == EM_TRICORE ||
        l->machine == EM_XTENSA || l->machine == EM_PPC ||
        l->machine == EM_RX || l->machine == EM_SPARC ||
        l->machine == EM_68K || l->machine == EM_ARM ||
        l->machine == EM_RISCV)
        va = align_up(va, 4);
    *data_filesz = va - *data_start;   /* .bss is beyond the file image */

    /* The part's flash is finite, and an image past its end does not fail
     * to run: on the ATmega328P the copy of .data read the bytes beyond
     * 32 KB, and a program that printed nothing but digits was the only
     * symptom. What is stored is the text and, in a firmware layout, the
     * initial data after it. */
    if (l->rom_limit) {
        Elf64_Addr end = l->data_base ? l->data_lma + *data_filesz
                                      : *text_start + *text_size;
        if (end - l->base > l->rom_limit)
            die("the image needs %lu bytes of flash and the part has %lu "
                "(--rom-limit): %lu of text, %lu of initial data",
                (unsigned long)(end - l->base), l->rom_limit,
                (unsigned long)*text_size,
                l->data_base ? (unsigned long)*data_filesz : 0UL);
    }

    b[OSEC_BSS].start = va;
    place_osec(l, OSEC_BSS, &va, b);   /* real .bss inputs first */
    /* then COMMON: each tentative symbol gets space, largest alignment
     * honored, and its value fixed to the reserved slot. */
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *g = &l->syms[i];
        if (!g->common && !g->was_common)
            continue;
        va = align_up(va, g->align ? g->align : 1);
        g->value = va;
        g->insec = -1;                 /* now an absolute address */
        g->common = 0;
        g->was_common = 1;             /* and placed again by a re-run */
        va += g->size;
    }
    if (l->machine == EM_MIPS || l->machine == EM_TRICORE ||
        l->machine == EM_XTENSA || l->machine == EM_PPC ||
        l->machine == EM_RX || l->machine == EM_SPARC ||
        l->machine == EM_68K)
        va = align_up(va, 4);
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
    /* Interned under a COPY of the name: callers build it in a buffer
     * they reuse, and the table keeps the pointer -- so __eh_frame_start,
     * which nothing referred to, was renamed __eh_frame_end when the
     * buffer was rewritten for the next one, and the image had two. */
    struct symbol *g = sym_find(l, name);
    if (!g)
        g = sym_intern(l, xstrndup(name, strlen(name)));
    /* only define it if something references it (it was interned) and it
     * is not already defined by a real object -- a real definition wins,
     * whether in a section, absolute (`.set _end, ADDR`) or COMMON. The
     * test was `insec >= 0`, which is a section, so an absolute _end was
     * replaced by the image's end without a word. The linker's own
     * definitions have no object, and are updated on each layout pass. */
    if ((g->defined || g->common) && g->obj)
        return;
    g->defined = 1;
    g->weak = 0;
    g->common = 0;
    g->insec = -1;
    g->value = value;
    /* RX: C names carry a leading underscore in the object (`__bss_start`
     * in C is `___bss_start`), so each linker symbol is given under that
     * spelling as well -- when something refers to it. */
    if (l->machine == EM_RX && strlen(name) < 250) {
        char un[256];
        un[0] = '_';
        memcpy(un + 1, name, strlen(name) + 1);
        if (sym_find(l, un)) {
            struct symbol *u = sym_find(l, un);
            if (!((u->defined || u->common) && u->obj)) {
                u->defined = 1;
                u->weak = 0;
                u->common = 0;
                u->insec = -1;
                u->value = value;
            }
        }
    }
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
        if (out < 0) {
            /* A DWARF section symbol. It has no ADDRESS -- the section
             * is not allocated -- but it does have a position in the
             * merged section, and that is what the reference means: a
             * CU's abbrev offset and its stmt_list are offsets into
             * .debug_abbrev and .debug_line, expressed as relocations
             * against those section symbols. Resolving them to this
             * object's own contribution is the whole of what merging
             * DWARF takes. */
            if (o->dbg_sec[sy->st_shndx] >= 0)
                return (Elf64_Addr)o->dbg_off[sy->st_shndx] + sy->st_value;
            die("%s: local symbol '%s' is in section %u (%s), which is not "
                "allocated and so has no address to relocate against",
                o->name, name && *name ? name : "<unnamed>",
                (unsigned)sy->st_shndx,
                o->shstr + o->shdrs[sy->st_shndx].sh_name);
        }
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
        die("%s: a Thumb branch is more than 16MB away, and so would be a "
            "veneer at the end of its output section", o->name);
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

static unsigned int get32loc(const unsigned char *p);

/* ---- A32 (ARM state) ----------------------------------------------------
 *
 * One little-endian word each. `b`, `bl` and `b<c>`: cond 101 L imm24, a
 * word displacement from the instruction + 8. movw/movt: the 16-bit
 * immediate split imm4 (bits 19:16) and imm12 (bits 11:0). */
static long long read_arm_b24(const unsigned char *loc)
{
    long long v = (long long)(get32loc(loc) & 0xffffffu);
    if (v & 0x800000)
        v -= 0x1000000;
    return v * 4;
}

/* `blx` (immediate) is `bl`'s ARM-to-Thumb twin: 1111 101 H imm24, H the
 * halfword bit of the displacement. A call relocation against a Thumb
 * function (its symbol odd) becomes one; a jump cannot. */
static void patch_arm_b24(struct object *o, unsigned char *loc, long long off,
                          int call, int thumb)
{
    unsigned int w = get32loc(loc);
    if (off < -(1LL << 25) || off >= (1LL << 25))
        die("%s: an ARM branch is more than 32MB away, and so would be a "
            "veneer at the end of its output section", o->name);
    if (thumb) {
        if (!call || (w >> 28) != 0xe)
            die("%s: a %s to a Thumb function from ARM code needs an "
                "interworking veneer, and none was minted for it",
                o->name, call ? "conditional call" : "branch");
        put32(loc, 0xfa000000u | (unsigned)((off >> 1) & 1) << 24 |
                   ((unsigned)(off >> 2) & 0xffffffu));
        return;
    }
    if (off & 3)
        die("%s: an ARM branch to an address that is not word-aligned",
            o->name);
    put32(loc, (w & 0xff000000u) | ((unsigned)(off >> 2) & 0xffffffu));
}

/* Is the target of a Thumb branch at `symi` an ARM-state function? Its
 * symbol is a defined global FUNCTION with bit 0 clear: every Thumb
 * function's symbol carries the bit, so an even one is A32 code. */
static int thumb_to_arm(struct linker *l, struct object *o, Elf64_Word symi,
                        Elf64_Addr S)
{
    Elf64_Sym *sy = &o->syms[symi];
    const char *name = o->symstr + sy->st_name;
    struct symbol *g;
    if ((S & 1) || ELF64_ST_BIND(sy->st_info) == STB_LOCAL || !*name)
        return 0;
    g = sym_find(l, name);
    return g && g->defined && g->type == STT_FUNC && g->insec >= 0;
}

static unsigned int read_arm_mov(const unsigned char *loc)
{
    unsigned int w = get32loc(loc);
    return ((w >> 4) & 0xf000u) | (w & 0xfffu);
}

static void patch_arm_mov(unsigned char *loc, unsigned int h)
{
    unsigned int w = get32loc(loc);
    put32(loc, (w & 0xfff0f000u) | ((h & 0xf000u) << 4) | (h & 0xfffu));
}

/* ---- RISC-V relocations -------------------------------------------------
 *
 * Four shapes, and only one of them is an ordinary field.
 *
 * HI20/LO12 are a pair in ARITHMETIC, not in bits: `lui` supplies bits
 * 31:12 and the paired instruction a SIGN-EXTENDED low 12, so when bit 11
 * of the address is set the low half contributes -4096..-1 and the high
 * half must be one larger. That is the +0x800 below, and it is the single
 * most-repeated bug in RISC-V toolchains: without it every address whose
 * bit 11 is set comes out 4096 too low, which is half of them.
 *
 * CALL patches TWO instructions from ONE relocation -- the `auipc` it
 * sits on and the `jalr` four bytes later -- and the same rounding
 * applies, with the displacement measured from the auipc.
 *
 * LO12_S is the same twelve bits as LO12_I in an S-type instruction,
 * where the field is split across bits 31:25 and 11:7 so that rs1 and rs2
 * keep the places they have in every other format.
 *
 * RELAX carries no value: it marks a site a linker MAY shorten. Optional,
 * so ignoring it is correct, and this does.
 */
static long rv_hi20(long long v) { return (long)(((v + 0x800) >> 12) & 0xfffff); }
static int  rv_lo12(long long v) { return (int)(((v & 0xfff) ^ 0x800) - 0x800); }

static void rv_put_u(unsigned char *loc, long hi)
{
    /* Only the imm field; the opcode and rd stay as the compiler wrote
     * them, so a relocation cannot quietly turn a `lui` into an `auipc`. */
    put32(loc, (get32loc(loc) & 0x00000fffU) |
               ((unsigned int)hi << 12));
}

static void rv_put_i(unsigned char *loc, int lo)
{
    put32(loc, (get32loc(loc) & 0x000fffffU) |
               (((unsigned int)lo & 0xfffU) << 20));
}

static void rv_put_s(unsigned char *loc, int lo)
{
    unsigned int u = (unsigned int)lo & 0xfffU;
    put32(loc, (get32loc(loc) & 0x01fff07fU) |
               ((u & 0x1fU) << 7) | ((u >> 5) << 25));
}

/* Every PCREL_HI20 this link has resolved, by the address of its auipc.
 * The low halves are looked up here rather than recomputed, because the
 * two must agree about the rounding and only the high one saw the whole
 * displacement. A linear scan: there are a handful per function and the
 * lookup is once per pair. */
static void note_pcrel_hi(struct linker *l, Elf64_Addr at, long long val)
{
    if (l->npcrel == l->cappcrel) {
        l->cappcrel = l->cappcrel ? l->cappcrel * 2 : 64;
        l->pcrel = xrealloc(l->pcrel, (size_t)l->cappcrel * sizeof *l->pcrel);
    }
    l->pcrel[l->npcrel].at = at;
    l->pcrel[l->npcrel].val = val;
    l->npcrel++;
}

static long long find_pcrel_hi(struct linker *l, struct object *o,
                               Elf64_Addr at)
{
    for (int i = l->npcrel - 1; i >= 0; i--)
        if (l->pcrel[i].at == at)
            return l->pcrel[i].val;
    die("%s: a RISC-V PCREL_LO12 relocation names 0x%llx, where no "
        "PCREL_HI20 was relocated; the two halves of an address must be "
        "emitted as a pair", o->name, (unsigned long long)at);
    return 0;
}

/* A relocated value its field cannot hold. Writing the low bits makes an
 * address nothing reports -- the field holds SOME address, and the
 * program goes there -- so it is refused, naming the symbol and the
 * reach. An x86-64 `mov $sym, %eax` linked above 4GB, or a RISC-V
 * `lui`/`auipc` pair asked for more than +-2GB, did exactly that. */
#define I32_MIN (-2147483647LL - 1)
#define I32_MAX 2147483647LL
static void need_range(struct linker *l, struct object *o, const char *rel,
                       long long v, long long lo, long long hi)
{
    if (v >= lo && v <= hi)
        return;
    die("%s: %s against '%s' needs %lld (0x%llx), and the field holds %lld "
        "to %lld; the image is laid out beyond what this code can reach",
        o->name, rel, l->rel_sym ? l->rel_sym : "?", v,
        (unsigned long long)v, lo, hi);
}

/* ---- RX ------------------------------------------------------------------
 *
 * RELA. The PC-relative fields are measured from the branch's opcode, one
 * byte before the field (binutils' elf32-rx.c adds the 1): S + A - P + 1.
 * Every field is little-endian. R_RX_RH_RELAX is a hint for a relaxing
 * linker, which this is not; the other linker-relaxation, small-data and
 * expression relocations GNU as can write are refused by name. */
static void apply_rx(struct linker *l, struct object *o, unsigned type,
                     unsigned char *loc, Elf64_Addr S, long long A,
                     Elf64_Addr P)
{
    long long V = (long long)S + A;
    long long d = V - (long long)P + 1;
    switch (type) {
    case R_RX_NONE:
    case R_RX_RH_RELAX:
        return;
    case R_RX_DIR32:
        need_range(l, o, "R_RX_DIR32", V, I32_MIN, 0xffffffffLL);
        put32(loc, (unsigned int)V);
        return;
    case R_RX_DIR24S_PCREL:
        need_range(l, o, "R_RX_DIR24S_PCREL", d, -0x800000LL, 0x7fffffLL);
        loc[0] = (unsigned char)d;
        loc[1] = (unsigned char)(d >> 8);
        loc[2] = (unsigned char)(d >> 16);
        return;
    case R_RX_DIR16S_PCREL:
        need_range(l, o, "R_RX_DIR16S_PCREL", d, -32768, 32767);
        loc[0] = (unsigned char)d;
        loc[1] = (unsigned char)(d >> 8);
        return;
    case R_RX_DIR8S_PCREL:
        need_range(l, o, "R_RX_DIR8S_PCREL", d, -128, 127);
        loc[0] = (unsigned char)d;
        return;
    case R_RX_DIR16: case R_RX_DIR16U: case R_RX_DIR16S:
        need_range(l, o, "R_RX_DIR16", V, -32768, 65535);
        loc[0] = (unsigned char)V;
        loc[1] = (unsigned char)(V >> 8);
        return;
    case R_RX_DIR8: case R_RX_DIR8U: case R_RX_DIR8S:
        need_range(l, o, "R_RX_DIR8", V, -128, 255);
        loc[0] = (unsigned char)V;
        return;
    default:
        die("%s: unsupported RX relocation type %u (a linker-relaxation, "
            "small-data or expression relocation; this linker applies "
            "R_RX_DIR32/24S/16/8 and their PC-relative forms)", o->name,
            type);
    }
}

static void apply_riscv(struct linker *l, struct object *o, unsigned type,
                        unsigned char *loc, Elf64_Addr S, long long A,
                        Elf64_Addr P)
{
    long long V = (long long)S + A;
    switch (type) {
    case R_RISCV_32:
        need_range(l, o, "R_RISCV_32", V, I32_MIN, 0xffffffffLL);
        put32(loc, (unsigned int)V);
        return;
    case R_RISCV_64:
        put64(loc, (unsigned long long)V);
        return;
    case R_RISCV_HI20:
        /* lui sign-extends at RV64, so the pair reaches a sign-extended
         * 32-bit address; at RV32 every address is one */
        if (!l->elf32)
            need_range(l, o, "R_RISCV_HI20", V, I32_MIN - 0x800,
                       I32_MAX - 0x800);
        rv_put_u(loc, rv_hi20(V));
        return;
    case R_RISCV_LO12_I:
        rv_put_i(loc, rv_lo12(V));
        return;
    case R_RISCV_LO12_S:
        rv_put_s(loc, rv_lo12(V));
        return;
    case R_RISCV_PCREL_HI20:
        /* An undefined weak symbol is 0, and an auipc in an image at
         * 0x80000000 cannot name 0: `&f == 0` for a weak f the program
         * does not define failed to link. The auipc becomes `lui rd, 0`
         * and its low half adds 0 -- an absolute zero, as GNU ld makes it.
         * (At RV32 the pair wraps with the address space and reaches.) */
        if (!l->elf32 && l->rel_uw && V == 0 &&
            (V - (long long)P < I32_MIN - 0x800 ||
             V - (long long)P > I32_MAX - 0x800)) {
            put32(loc, (get32loc(loc) & 0xf80U) | 0x37U);
            note_pcrel_hi(l, P, 0);
            return;
        }
        /* Recorded as well as written: the low half that pairs with it
         * has to take the low twelve bits of THIS displacement, not of
         * the address, or the two disagree about the +0x800 rounding
         * and the result is 4096 out for half of all symbols. */
        if (!l->elf32)
            need_range(l, o, "R_RISCV_PCREL_HI20", V - (long long)P,
                       I32_MIN - 0x800, I32_MAX - 0x800);
        note_pcrel_hi(l, P, V - (long long)P);
        rv_put_u(loc, rv_hi20(V - (long long)P));
        return;
    case R_RISCV_PCREL_LO12_I:
    case R_RISCV_PCREL_LO12_S: {
        /* S + A is the address of the AUIPC, not of the target: the
         * psABI resolves this relocation by looking up the high half's
         * own relocation there. */
        long long d = find_pcrel_hi(l, o, (Elf64_Addr)V);
        if (type == R_RISCV_PCREL_LO12_I) rv_put_i(loc, rv_lo12(d));
        else                              rv_put_s(loc, rv_lo12(d));
        return;
    }
    case R_RISCV_BRANCH:
    case R_RISCV_JAL: {
        /* An ordinary displacement, but in the two SCRAMBLED formats:
         * B-type stores bits (12, 10:5, 4:1, 11) and J-type
         * (20, 10:1, 11, 19:12), so the field is rebuilt by the same
         * encoder the compiler uses rather than shifted into place here.
         * EmbCC's own branches never reach this -- they are resolved
         * inside the function that emits them -- but clang's do, from a
         * `.L0` label in another section. */
        long long d = V - (long long)P;
        unsigned int keep = get32loc(loc);
        /* checked here: the encoders take it for a compiler bug */
        if (type == R_RISCV_BRANCH)
            need_range(l, o, "R_RISCV_BRANCH", d, -4096, 4094);
        else
            need_range(l, o, "R_RISCV_JAL", d, -(1LL << 20), (1LL << 20) - 2);
        if (type == R_RISCV_BRANCH)
            put32(loc, (keep & ~0xfe000f80U) |
                       (unsigned int)rv_enc_b(0, 0, 0, 0, (int)d));
        else
            put32(loc, (keep & ~0xfffff000U) |
                       (unsigned int)rv_enc_j(0, 0, (int)d));
        return;
    }
    case R_RISCV_CALL:
    case R_RISCV_CALL_PLT: {
        long long d = V - (long long)P;
        /* A call to an undefined weak function -- one `if (f) f();`
         * never makes -- goes to address 0 as a call through a null
         * pointer would: `lui ra, 0; jalr ra, 0(ra)`, where the auipc
         * could not reach (the PCREL_HI20 case above). */
        if (!l->elf32 && l->rel_uw && V == 0 &&
            (d < I32_MIN - 0x800 || d > I32_MAX - 0x800)) {
            put32(loc, (get32loc(loc) & 0xf80U) | 0x37U);
            rv_put_i(loc + 4, 0);
            return;
        }
        /* At RV32 the pair wraps with the address space, so every
         * address is in reach. At RV64 it is +-2GB, less the rounding
         * the low half's sign costs, and this linker mints no stubs. */
        if (!l->elf32)
            need_range(l, o, "R_RISCV_CALL", d, I32_MIN - 0x800,
                       I32_MAX - 0x800);
        rv_put_u(loc, rv_hi20(d));
        rv_put_i(loc + 4, rv_lo12(d));
        return;
    }
    case R_RISCV_RELAX:
        return;                 /* a hint; relaxation is optional */
    default:
        die("%s: unsupported RISC-V relocation type %u (this is the next "
            "linker increment, not a bug in your program)", o->name, type);
    }
}

/* ---- LoongArch64 ---------------------------------------------------------
 *
 * RELA, every field a plain bit range of one instruction word (the 21- and
 * 26-bit branch offsets split in two). EmbCC's objects carry B26 and the
 * PCALA pair; clang's also carry its default MEDIUM code model's CALL36
 * (pcaddu18i + jirl) and, for a symbol another object defines, the GOT
 * pair. This linker builds no GOT: a GOT access -- `pcalau12i rd,
 * %got_pc_hi20(s)` then `ld.d rd, rj, %got_pc_lo12(s)` -- is rewritten to
 * the direct address, `pcalau12i` of the symbol's page and an `addi.d` of
 * its low 12 bits, which is the relaxation GNU ld and lld apply whenever
 * the symbol is in the image (and on bare metal it always is). RELAX and
 * ALIGN are hints for a relaxing linker; this one moves nothing, which
 * leaves every distance the assembler measured as it was.
 * docs/internals/loongarch64-plan.md. */

/* Put `v` (already reduced to the field's width) into bits lo..hi of the
 * word at loc, leaving every other bit -- the opcode, the registers -- as
 * the compiler wrote them. */
static void la_put(unsigned char *loc, int lo, int width, unsigned long long v)
{
    unsigned int m = (unsigned int)(((1ULL << width) - 1) << lo);
    put32(loc, (get32loc(loc) & ~m) |
               ((unsigned int)(v << lo) & m));
}

static void apply_loongarch(struct linker *l, struct object *o, unsigned type,
                            unsigned char *loc, Elf64_Addr S, long long A,
                            Elf64_Addr P)
{
    long long V = (long long)S + A, d = V - (long long)P;
    switch (type) {
    case R_LARCH_NONE:
    case R_LARCH_RELAX:
    case R_LARCH_ALIGN:
        return;
    case R_LARCH_32:
        need_range(l, o, "R_LARCH_32", V, I32_MIN, 0xffffffffLL);
        put32(loc, (unsigned int)V);
        return;
    case R_LARCH_64:
        put64(loc, (unsigned long long)V);
        return;
    case R_LARCH_32_PCREL:
        need_range(l, o, "R_LARCH_32_PCREL", d, I32_MIN, I32_MAX);
        put32(loc, (unsigned int)d);
        return;
    case R_LARCH_64_PCREL:
        put64(loc, (unsigned long long)d);
        return;
    /* In-place arithmetic: a difference of two labels in data (a debug
     * line program's lengths, a jump table of offsets) is written as an
     * ADD of one symbol and a SUB of the other at the same place. */
    case R_LARCH_ADD8:  loc[0] = (unsigned char)(loc[0] + V); return;
    case R_LARCH_SUB8:  loc[0] = (unsigned char)(loc[0] - V); return;
    case R_LARCH_ADD16: put16(loc, (get16(loc) + (unsigned int)V) & 0xffffU);
                        return;
    case R_LARCH_SUB16: put16(loc, (get16(loc) - (unsigned int)V) & 0xffffU);
                        return;
    case R_LARCH_ADD32: put32(loc, get32loc(loc) + (unsigned int)V); return;
    case R_LARCH_SUB32: put32(loc, get32loc(loc) - (unsigned int)V); return;
    case R_LARCH_ADD6:
        loc[0] = (unsigned char)((loc[0] & 0xc0) | ((loc[0] + V) & 0x3f));
        return;
    case R_LARCH_SUB6:
        loc[0] = (unsigned char)((loc[0] & 0xc0) | ((loc[0] - V) & 0x3f));
        return;
    case R_LARCH_ADD64: case R_LARCH_SUB64: {
        unsigned long long w = (unsigned long long)get32loc(loc) |
                               ((unsigned long long)get32loc(loc + 4) << 32);
        w = type == R_LARCH_ADD64 ? w + (unsigned long long)V
                                  : w - (unsigned long long)V;
        put64(loc, w);
        return;
    }
    case R_LARCH_B16:
        if (d & 3)
            die("%s: R_LARCH_B16 against '%s' is not a multiple of four",
                o->name, l->rel_sym ? l->rel_sym : "?");
        need_range(l, o, "R_LARCH_B16", d, -(1LL << 17), (1LL << 17) - 4);
        la_put(loc, 10, 16, (unsigned long long)(d >> 2));
        return;
    case R_LARCH_B21:
        if (d & 3)
            die("%s: R_LARCH_B21 against '%s' is not a multiple of four",
                o->name, l->rel_sym ? l->rel_sym : "?");
        need_range(l, o, "R_LARCH_B21", d, -(1LL << 22), (1LL << 22) - 4);
        la_put(loc, 10, 16, (unsigned long long)(d >> 2));
        la_put(loc, 0, 5, (unsigned long long)(d >> 18));
        return;
    case R_LARCH_B26:
        if (d & 3)
            die("%s: R_LARCH_B26 against '%s' is not a multiple of four",
                o->name, l->rel_sym ? l->rel_sym : "?");
        need_range(l, o, "R_LARCH_B26", d, -(1LL << 27), (1LL << 27) - 4);
        la_put(loc, 10, 16, (unsigned long long)(d >> 2));
        la_put(loc, 0, 10, (unsigned long long)(d >> 18));
        return;
    case R_LARCH_CALL36: {
        /* pcaddu18i ra, hi20 ; jirl ra, ra, lo16 -- the jirl's offset is
         * sign-extended, so the high part is rounded by half its unit */
        long long hi;
        if (d & 3)
            die("%s: R_LARCH_CALL36 against '%s' is not a multiple of four",
                o->name, l->rel_sym ? l->rel_sym : "?");
        need_range(l, o, "R_LARCH_CALL36", d, -(1LL << 37) - 0x20000,
                   (1LL << 37) - 0x20000 - 4);
        hi = (d + 0x20000) >> 18;
        la_put(loc, 5, 20, (unsigned long long)hi);
        la_put(loc + 4, 10, 16, (unsigned long long)((d - (hi << 18)) >> 2));
        return;
    }
    case R_LARCH_GOT_PC_HI20:
    case R_LARCH_PCALA_HI20: {
        /* the page of the symbol, rounded by 0x800 because the low half
         * is sign-extended where it is added, less the page of pc */
        long long pg = (long long)(((unsigned long long)V + 0x800) & ~0xfffULL) -
                       (long long)(P & ~(Elf64_Addr)0xfff);
        need_range(l, o, type == R_LARCH_PCALA_HI20 ? "R_LARCH_PCALA_HI20"
                                                    : "R_LARCH_GOT_PC_HI20",
                   pg, I32_MIN, I32_MAX - 0xfff);
        la_put(loc, 5, 20, (unsigned long long)(pg >> 12));
        return;
    }
    case R_LARCH_GOT_PC_LO12: {
        /* ld.d rd, rj, %got_pc_lo12 -> addi.d rd, rj, %pc_lo12: the
         * address itself rather than a GOT slot holding it */
        unsigned int w = get32loc(loc);
        if ((w & 0xffc00000U) != 0x28c00000U)
            die("%s: R_LARCH_GOT_PC_LO12 against '%s' is on 0x%08x, not an "
                "ld.d; this linker builds no GOT and rewrites only the normal "
                "and medium models' ld.d (-mcmodel=extreme is not linked)",
                o->name, l->rel_sym ? l->rel_sym : "?", w);
        put32(loc, (w & 0x003fffffU) | 0x02c00000U);
    }
        /* fall through */
    case R_LARCH_PCALA_LO12:
    case R_LARCH_ABS_LO12:
        la_put(loc, 10, 12, (unsigned long long)V & 0xfff);
        return;
    case R_LARCH_ABS_HI20:
        /* lu12i.w then ori: no rounding, the ori ORs in the low 12 bits.
         * Alone (no lu32i.d after it) the pair reaches a sign-extended
         * 32-bit address; that is the instruction's to know, not this
         * field's, so only the field is checked here. */
        la_put(loc, 5, 20, ((unsigned long long)V >> 12) & 0xfffff);
        return;
    case R_LARCH_ABS64_LO20:
        la_put(loc, 5, 20, ((unsigned long long)V >> 32) & 0xfffff);
        return;
    case R_LARCH_ABS64_HI12:
        la_put(loc, 10, 12, ((unsigned long long)V >> 52) & 0xfff);
        return;
    case R_LARCH_PCREL20_S2:
        if (d & 3)
            die("%s: R_LARCH_PCREL20_S2 against '%s' is not a multiple of "
                "four", o->name, l->rel_sym ? l->rel_sym : "?");
        need_range(l, o, "R_LARCH_PCREL20_S2", d, -(1LL << 21),
                   (1LL << 21) - 4);
        la_put(loc, 5, 20, (unsigned long long)(d >> 2));
        return;
    case R_LARCH_PCALA64_LO20: case R_LARCH_PCALA64_HI12:
    case R_LARCH_GOT64_PC_LO20: case R_LARCH_GOT64_PC_HI12:
    case R_LARCH_GOT_HI20: case R_LARCH_GOT_LO12:
        die("%s: LoongArch relocation type %u against '%s' is the extreme "
            "code model's or an absolute GOT's; this linker links the normal "
            "and medium models (-mcmodel=normal or medium)", o->name, type,
            l->rel_sym ? l->rel_sym : "?");
    default:
        die("%s: unsupported LoongArch relocation type %u against '%s' (TLS "
            "and ULEB128 differences are not linked yet)", o->name, type,
            l->rel_sym ? l->rel_sym : "?");
    }
}

/* ---- MIPS (o32) --------------------------------------------------------
 *
 * The objects are REL: each addend is in the field it relocates, in that
 * field's shape. HI16 and LO16 share ONE addend between them -- AHL, the
 * HI16 field shifted up plus the LO16 field sign-extended -- so a HI16 is
 * applied with the next LO16 against the same symbol in its section (the
 * AHL rule; docs/internals/mips32-plan.md). The high half is rounded by
 * 0x8000 because the low half is sign-extended when it is added.
 *
 * The GOT and GP-relative types are what PIC and small-data code use;
 * this linker builds neither a GOT nor a _gp, and says so by name. */
struct mips_relctx {
    const unsigned char *rbytes;   /* the relocation section */
    int j, n, relsz, isrel;
    const unsigned char *base;     /* the relocated section's bytes */
    Elf64_Word symi;
    int local;                     /* the symbol is STB_LOCAL */
};

static long long sext(unsigned long long v, int bits)
{
    unsigned long long m = 1ULL << (bits - 1);
    v &= (1ULL << bits) - 1;
    return (long long)((v ^ m) - m);
}

/* The LO16 that pairs with the HI16 at index c->j: its sign-extended
 * field, the low half of AHL. */
static long long mips_lo_of(struct object *o, const struct mips_relctx *c)
{
    for (int k = c->j + 1; k < c->n; k++) {
        const Elf32_Rel *r = (const Elf32_Rel *)(c->rbytes +
                                                (long)k * c->relsz);
        if ((r->r_info & 0xff) == R_MIPS_LO16 && (r->r_info >> 8) == c->symi)
            return sext(mips_get_word(c->base + r->r_offset) & 0xffffU, 16);
    }
    die("%s: an R_MIPS_HI16 with no R_MIPS_LO16 against the same symbol "
        "after it; the two halves of an address carry one addend between "
        "them and cannot be resolved apart", o->name);
    return 0;
}

/* Every MIPS field is a word in the image's byte order (mips_get_word,
 * set from the objects), and its 16-bit immediate the word's low half. */
static void mips_put16(unsigned char *loc, unsigned long v)
{
    mips_put_word(loc, (mips_get_word(loc) & 0xffff0000U) |
                       (unsigned int)(v & 0xffffU));
}

static void apply_mips(struct linker *l, struct object *o, unsigned type,
                       unsigned char *loc, Elf64_Addr S, long long A,
                       Elf64_Addr P, const struct mips_relctx *c)
{
    unsigned int field = (unsigned int)mips_get_word(loc);
    long long V;
    switch (type) {
    case R_MIPS_NONE:
    case R_MIPS_JALR:             /* a hint that a jalr may become a bal */
        return;
    case R_MIPS_32:
        if (c->isrel) A = (long long)(int)field;
        V = (long long)S + A;
        need_range(l, o, "R_MIPS_32", V, I32_MIN, 0xffffffffLL);
        mips_put_word(loc, (unsigned int)V);
        return;
    case R_MIPS_HI16:
        if (c->isrel)
            A = ((long long)(field & 0xffffU) << 16) + mips_lo_of(o, c);
        V = (long long)S + A;
        mips_put16(loc, (unsigned long)((V + 0x8000) >> 16));
        return;
    /* n64's upper pieces of a 64-bit address (RELA), each rounded for
     * the sign extension of every piece added after it */
    case R_MIPS_HIGHER:
        V = (long long)S + A;
        mips_put16(loc, (unsigned long)(((unsigned long long)V +
                                         0x80008000ULL) >> 32));
        return;
    case R_MIPS_HIGHEST:
        V = (long long)S + A;
        mips_put16(loc, (unsigned long)(((unsigned long long)V +
                                         0x800080008000ULL) >> 48));
        return;
    case R_MIPS_64:
        if (c->isrel)
            die("%s: an R_MIPS_64 in a REL section", o->name);
        V = (long long)S + A;
        for (int b = 0; b < 8; b++)
            loc[mips_big_endian() ? 7 - b : b] =
                (unsigned char)((unsigned long long)V >> (8 * b));
        return;
    case R_MIPS_LO16:
        if (c->isrel) A = sext(field & 0xffffU, 16);
        V = (long long)S + A;
        mips_put16(loc, (unsigned long)V);
        return;
    case R_MIPS_26: {
        /* A 26-bit word index within the 256 MiB region of the delay
         * slot. A local symbol's addend is unsigned (its region bits come
         * from the place), an external one's sign-extended. */
        long long a = c->isrel ? (long long)((field & 0x3ffffffU) << 2) : A;
        if (c->isrel && !c->local)
            a = sext((unsigned long long)a, 28);
        V = (long long)S + a;
        if (V & 3)
            die("%s: a jal or j to 0x%llx, which is not a multiple of 4 "
                "(against '%s')", o->name, (unsigned long long)V,
                l->rel_sym ? l->rel_sym : "?");
        if ((((unsigned long long)V ^ (unsigned long long)(P + 4)) &
             0xf0000000ULL) != 0)
            die("%s: a jal or j at 0x%llx to '%s' at 0x%llx, which is in "
                "another 256 MiB region; jal reaches only its own",
                o->name, (unsigned long long)P,
                l->rel_sym ? l->rel_sym : "?", (unsigned long long)V);
        mips_put_word(loc, (field & 0xfc000000U) |
                   (unsigned int)(((unsigned long long)V >> 2) & 0x3ffffffU));
        return;
    }
    case R_MIPS_PC16: {
        long long d;
        if (c->isrel) A = sext((unsigned long long)(field & 0xffffU) << 2, 18);
        d = (long long)S + A - (long long)P;
        need_range(l, o, "R_MIPS_PC16", d, -131072, 131068);
        mips_put16(loc, (unsigned long)(d >> 2));
        return;
    }
    case R_MIPS_GPREL16: case R_MIPS_GPREL32: case R_MIPS_LITERAL:
        die("%s: relocation %s: small-data (gp-relative) addressing, which "
            "this linker does not lay out; compile it with -G0", o->name,
            type == R_MIPS_LITERAL ? "R_MIPS_LITERAL" : type == R_MIPS_GPREL32
            ? "R_MIPS_GPREL32" : "R_MIPS_GPREL16");
        return;
    case R_MIPS_GOT16: case R_MIPS_CALL16:
        die("%s: relocation %s: position-independent code through a GOT, "
            "which this linker does not build; compile it without -fPIC "
            "(-mno-abicalls, or clang's default -fno-pic)", o->name,
            type == R_MIPS_GOT16 ? "R_MIPS_GOT16" : "R_MIPS_CALL16");
        return;
    default:
        die("%s: unsupported MIPS relocation type %u (this is the next "
            "linker increment, not a bug in your program)", o->name, type);
    }
}

/* ---- TriCore -------------------------------------------------------------
 *
 * RELA, the addend in the relocation (docs/internals/tricore-plan.md).
 * An address is two halves in two instruction formats: HIADJ in an RLC
 * const16 (MOVH, MOVH.A), rounded by 0x8000 because the low half is
 * sign-extended where it is added; LO in an RLC const16 (ADDI); LO2 in a
 * BOL long offset (LEA, a load, a store), whose 16 bits are scattered in
 * three pieces. 24REL is CALL's and J's halfword displacement. Every
 * field is rebuilt by src/arch/tricore/emit.c's format packers from the
 * instruction's own other fields, so no layout is written down here. The
 * small-data and other types are refused by name. */
static void apply_tricore(struct linker *l, struct object *o, unsigned type,
                          unsigned char *loc, Elf64_Addr S, long long A,
                          Elf64_Addr P)
{
    unsigned long w = get32loc(loc);
    int op1 = (int)(w & 0xff), s1 = (int)((w >> 8) & 15),
        s2 = (int)((w >> 12) & 15), d = (int)((w >> 28) & 15);
    long long V = (long long)S + A;
    switch (type) {
    case R_TRICORE_NONE:
        return;
    case R_TRICORE_32ABS:
        need_range(l, o, "R_TRICORE_32ABS", V, I32_MIN, 0xffffffffLL);
        put32(loc, (unsigned int)V);
        return;
    case R_TRICORE_24REL: {
        long long dd = V - (long long)P;
        if (op1 != 0x1d && op1 != 0x6d && op1 != 0x5d)
            die("%s: R_TRICORE_24REL on 0x%08lx, which is not a J, CALL or "
                "JL", o->name, w);
        if (dd & 1)
            die("%s: a call or jump to the odd address 0x%llx ('%s')",
                o->name, (unsigned long long)V, l->rel_sym ? l->rel_sym : "?");
        need_range(l, o, "R_TRICORE_24REL", dd, -16777216LL, 16777214LL);
        put32(loc, (unsigned int)tc_enc_b(op1, (unsigned long)(dd / 2) &
                                               0xffffffUL));
        return;
    }
    case R_TRICORE_HIADJ:
        put32(loc, (unsigned int)tc_enc_rlc(op1, d, s1,
                                            tc_hi_adj((unsigned long)V)));
        return;
    case R_TRICORE_LO:
        put32(loc, (unsigned int)tc_enc_rlc(op1, d, s1,
                                            (unsigned)(V & 0xffff)));
        return;
    case R_TRICORE_LO2:
        put32(loc, (unsigned int)tc_enc_bol(op1, s1, s2,
                                            (unsigned)(V & 0xffff)));
        return;
    case R_TRICORE_16SM: case R_TRICORE_10SM:
        die("%s: relocation %s: small-data (A0-relative) addressing, which "
            "this linker does not lay out", o->name,
            type == R_TRICORE_16SM ? "R_TRICORE_16SM" : "R_TRICORE_10SM");
        return;
    default:
        die("%s: unsupported TriCore relocation type %u (this is the next "
            "linker increment, not a bug in your program)", o->name, type);
    }
}

/* ---- AVR --------------------------------------------------------------
 *
 * Two things make this machine's relocations unlike the others'.
 *
 * An address arrives a BYTE at a time, because the registers are eight
 * bits wide: LO8_LDI and HI8_LDI patch two separate `ldi` instructions
 * with two halves of one sixteen-bit value. They are independent sites,
 * so unlike RISC-V's auipc/addi pair there is no rounding to agree on and
 * no need to remember the first when applying the second.
 *
 * And program space is a SEPARATE address space addressed in WORDS. The
 * _GS and _PM forms halve the address; the plain forms do not. Using the
 * wrong one does not fault -- it produces a pointer to twice as far into
 * flash, which lands on a real instruction -- so the halving is the whole
 * difference between a working indirect call and a program that runs the
 * wrong function.
 *
 * Every split field is patched by a function in src/arch/avr/emit.c, the
 * same one the encoder writes through. None of those layouts is written
 * down here.
 */
/* PowerPC (SVR4/EABI): RELA, so the addend is in the entry and every
 * field is written whole. The fields are big-endian words and halfwords
 * in the image (ppc_get_word); a 16-bit relocation's r_offset names the
 * HALFWORD, an instruction's low half. ADDR16_HA is the high half
 * adjusted for the sign of the low one, which addi adds sign-extended.
 * The small-data and GOT/PLT types are what -msdata and PIC code use;
 * this linker lays out neither, and says so by name. */
static void put16be(unsigned char *loc, unsigned long v)
{
    loc[0] = (unsigned char)(v >> 8);
    loc[1] = (unsigned char)v;
}

static void apply_ppc(struct linker *l, struct object *o, unsigned type,
                      unsigned char *loc, Elf64_Addr S, long long A,
                      Elf64_Addr P)
{
    long long V = (long long)S + A, d;
    unsigned long w;
    switch (type) {
    case R_PPC_NONE:
        return;
    case R_PPC_ADDR32:
        need_range(l, o, "R_PPC_ADDR32", V, I32_MIN, 0xffffffffLL);
        ppc_put_word(loc, (unsigned long)V & 0xffffffffUL);
        return;
    case R_PPC_REL32:
        d = V - (long long)P;
        need_range(l, o, "R_PPC_REL32", d, I32_MIN, 0x7fffffffLL);
        ppc_put_word(loc, (unsigned long)d & 0xffffffffUL);
        return;
    case R_PPC_ADDR16:
        need_range(l, o, "R_PPC_ADDR16", V, -32768, 65535);
        put16be(loc, (unsigned long)V);
        return;
    case R_PPC_ADDR16_LO:
        put16be(loc, (unsigned long)V);
        return;
    case R_PPC_ADDR16_HI:
        put16be(loc, (unsigned long)(V >> 16));
        return;
    case R_PPC_ADDR16_HA:
        put16be(loc, (unsigned long)((V + 0x8000) >> 16));
        return;
    case R_PPC_REL24: case R_PPC_PLTREL24:
        /* b/bl: a word displacement in bits 6..29, +-32 MiB of the branch */
        d = V - (long long)P;
        if (d & 3)
            die("%s: a branch at 0x%llx to '%s' at 0x%llx, which is not a "
                "multiple of 4", o->name, (unsigned long long)P,
                l->rel_sym ? l->rel_sym : "?", (unsigned long long)V);
        need_range(l, o, type == R_PPC_REL24 ? "R_PPC_REL24"
                                             : "R_PPC_PLTREL24",
                   d, -(1LL << 25), (1LL << 25) - 4);
        w = ppc_get_word(loc);
        ppc_put_word(loc, (w & ~0x3fffffcUL) |
                          ((unsigned long)d & 0x3fffffcUL));
        return;
    case R_PPC_ADDR24:
        if (V & 3)
            die("%s: an absolute branch to 0x%llx, not a multiple of 4",
                o->name, (unsigned long long)V);
        need_range(l, o, "R_PPC_ADDR24", V, -(1LL << 25), (1LL << 25) - 4);
        w = ppc_get_word(loc);
        ppc_put_word(loc, (w & ~0x3fffffcUL) |
                          ((unsigned long)V & 0x3fffffcUL));
        return;
    case R_PPC_REL14:
        d = V - (long long)P;
        if (d & 3)
            die("%s: a conditional branch to an address not a multiple of 4",
                o->name);
        need_range(l, o, "R_PPC_REL14", d, -32768, 32764);
        w = ppc_get_word(loc);
        ppc_put_word(loc, (w & ~0xfffcUL) | ((unsigned long)d & 0xfffcUL));
        return;
    case R_PPC_SDAREL16: case R_PPC_EMB_SDA21:
        die("%s: relocation %s: small-data (r2/r13-relative) addressing, "
            "which this linker does not lay out; compile it with "
            "-msdata=none (-G0)", o->name,
            type == R_PPC_SDAREL16 ? "R_PPC_SDAREL16" : "R_PPC_EMB_SDA21");
        return;
    case R_PPC_GOT16:
        die("%s: relocation R_PPC_GOT16: position-independent code through "
            "a GOT, which this linker does not build; compile it without "
            "-fPIC", o->name);
        return;
    default:
        die("%s: unsupported PowerPC relocation type %u (this is the next "
            "linker increment, not a bug in your program)", o->name, type);
    }
}

/* SPARC: RELA, so the addend is in the entry and every field is written
 * whole, a big-endian word in the image (sparc_get_word). HI22 is sethi's
 * high 22 bits and LO10 the low ten that an `or` or a load's offset adds
 * back -- they never carry, so neither is rounded. WDISP30 is call's word
 * displacement (it reaches anywhere in 32 bits), WDISP22 a branch's. The
 * GOT and PLT types are what PIC code uses, which this linker does not
 * lay out, and it says so by name. */
static void apply_sparc(struct linker *l, struct object *o, unsigned type,
                        unsigned char *loc, Elf64_Addr S, long long A,
                        Elf64_Addr P)
{
    long long V = (long long)S + A, d;
    unsigned long w;
    switch (type) {
    case R_SPARC_NONE:
        return;
    case R_SPARC_32: case R_SPARC_UA32:
        need_range(l, o, "R_SPARC_32", V, I32_MIN, 0xffffffffLL);
        sparc_put_word(loc, (unsigned long)V & 0xffffffffUL);
        return;
    case R_SPARC_DISP32:
        d = V - (long long)P;
        need_range(l, o, "R_SPARC_DISP32", d, I32_MIN, 0x7fffffffLL);
        sparc_put_word(loc, (unsigned long)d & 0xffffffffUL);
        return;
    case R_SPARC_16:
        need_range(l, o, "R_SPARC_16", V, -32768, 65535);
        loc[0] = (unsigned char)(V >> 8);
        loc[1] = (unsigned char)V;
        return;
    case R_SPARC_8:
        need_range(l, o, "R_SPARC_8", V, -128, 255);
        loc[0] = (unsigned char)V;
        return;
    case R_SPARC_HI22:
        need_range(l, o, "R_SPARC_HI22", V, I32_MIN, 0xffffffffLL);
        w = sparc_get_word(loc);
        sparc_put_word(loc, (w & ~0x3fffffUL) |
                            (((unsigned long)V >> 10) & 0x3fffffUL));
        return;
    case R_SPARC_LO10:
        w = sparc_get_word(loc);
        sparc_put_word(loc, (w & ~0x3ffUL) | ((unsigned long)V & 0x3ffUL));
        return;
    case R_SPARC_13:
        need_range(l, o, "R_SPARC_13", V, -4096, 4095);
        w = sparc_get_word(loc);
        sparc_put_word(loc, (w & ~0x1fffUL) | ((unsigned long)V & 0x1fffUL));
        return;
    case R_SPARC_WDISP30: case R_SPARC_WPLT30:
        d = V - (long long)P;
        if (d & 3)
            die("%s: a call at 0x%llx to '%s' at 0x%llx, which is not a "
                "multiple of 4", o->name, (unsigned long long)P,
                l->rel_sym ? l->rel_sym : "?", (unsigned long long)V);
        w = sparc_get_word(loc);
        sparc_put_word(loc, (w & ~0x3fffffffUL) |
                            ((unsigned long)(d >> 2) & 0x3fffffffUL));
        return;
    case R_SPARC_WDISP22:
        d = V - (long long)P;
        if (d & 3)
            die("%s: a branch at 0x%llx to an address not a multiple of 4",
                o->name, (unsigned long long)P);
        need_range(l, o, "R_SPARC_WDISP22", d, -(1LL << 23),
                   (1LL << 23) - 4);
        w = sparc_get_word(loc);
        sparc_put_word(loc, (w & ~0x3fffffUL) |
                            ((unsigned long)(d >> 2) & 0x3fffffUL));
        return;
    case R_SPARC_GOT10: case R_SPARC_GOT13: case R_SPARC_GOT22:
    case R_SPARC_PC10: case R_SPARC_PC22:
        die("%s: relocation type %u: position-independent code (a GOT or "
            "a PC-relative address), which this linker does not lay out; "
            "compile it without -fPIC", o->name, type);
        return;
    default:
        die("%s: unsupported SPARC relocation type %u (this is the next "
            "linker increment, not a bug in your program)", o->name, type);
    }
}

/* m68k / ColdFire: RELA, every field big-endian and written whole. EmbCC's
 * objects carry R_68K_32 for every address and call (the 32-bit operand
 * after the operation word); the PC-relative forms are what an assembler
 * writes for a bsr or a (d16,pc) operand, measured from the field. The
 * GOT and PLT forms are position-independent code, refused by name. */
static void put_be(unsigned char *loc, int n, unsigned long v)
{
    for (int k = 0; k < n; k++)
        loc[k] = (unsigned char)(v >> (8 * (n - 1 - k)));
}

static void apply_m68k(struct linker *l, struct object *o, unsigned type,
                       unsigned char *loc, Elf64_Addr S, long long A,
                       Elf64_Addr P)
{
    long long V = (long long)S + A, d = V - (long long)P;
    switch (type) {
    case R_68K_NONE:
        return;
    case R_68K_32:
        need_range(l, o, "R_68K_32", V, I32_MIN, 0xffffffffLL);
        put_be(loc, 4, (unsigned long)V & 0xffffffffUL);
        return;
    case R_68K_16:
        need_range(l, o, "R_68K_16", V, -32768, 65535);
        put_be(loc, 2, (unsigned long)V & 0xffffUL);
        return;
    case R_68K_8:
        need_range(l, o, "R_68K_8", V, -128, 255);
        put_be(loc, 1, (unsigned long)V & 0xffUL);
        return;
    case R_68K_PC32:
        need_range(l, o, "R_68K_PC32", d, I32_MIN, 0x7fffffffLL);
        put_be(loc, 4, (unsigned long)d & 0xffffffffUL);
        return;
    case R_68K_PC16:
        need_range(l, o, "R_68K_PC16", d, -32768, 32767);
        put_be(loc, 2, (unsigned long)d & 0xffffUL);
        return;
    case R_68K_PC8:
        need_range(l, o, "R_68K_PC8", d, -128, 127);
        put_be(loc, 1, (unsigned long)d & 0xffUL);
        return;
    case R_68K_GOT32: case R_68K_PLT32:
        die("%s: relocation %s: position-independent code through a GOT "
            "or PLT, which this linker does not build; compile it without "
            "-fPIC", o->name,
            type == R_68K_GOT32 ? "R_68K_GOT32" : "R_68K_PLT32");
        return;
    default:
        die("%s: unsupported m68k relocation type %u (this is the next "
            "linker increment, not a bug in your program)", o->name, type);
    }
}

static void apply_avr(struct linker *l, struct object *o, unsigned type,
                      unsigned char *loc, Elf64_Addr S, long long A,
                      Elf64_Addr P)
{
    long long V = (long long)S + A;
    (void)l;
    switch (type) {
    case R_AVR_NONE:
        return;
    case R_AVR_32:
        put32(loc, (unsigned int)V);
        return;
    case R_AVR_16:
        put16(loc, (unsigned int)V & 0xffffu);
        return;
    case R_AVR_16_PM:
        /* A function pointer in data: the WORD address. */
        put16(loc, (unsigned int)(V >> 1) & 0xffffu);
        return;
    case R_AVR_LO8_LDI:
        avr_patch_ldi_at(loc, (int)(V & 0xff));
        return;
    case R_AVR_HI8_LDI:
        avr_patch_ldi_at(loc, (int)((V >> 8) & 0xff));
        return;
    case R_AVR_LO8_LDI_GS:
        avr_patch_ldi_at(loc, (int)((V >> 1) & 0xff));
        return;
    case R_AVR_HI8_LDI_GS:
        avr_patch_ldi_at(loc, (int)((V >> 9) & 0xff));
        return;
    case R_AVR_CALL:
        if (V & 1)
            die("%s: a call to an odd address 0x%llx; AVR instructions are "
                "halfword-aligned and the address is halved to a word "
                "number, so an odd one cannot be encoded", o->name,
                (unsigned long long)V);
        avr_patch_call_at(loc, (long)V);
        return;
    case R_AVR_13_PCREL: {
        /* rjmp/rcall: words, from the instruction AFTER. */
        long long d = (V - (long long)P - 2) / 2;
        if (d < -2048 || d > 2047)
            die("%s: an rjmp reaches +-4KB and this target is %lld bytes "
                "away; this linker mints no trampolines, so the call has "
                "to be a `call` rather than an `rcall`", o->name,
                (long long)(V - (long long)P));
        avr_patch_rjmp_at(loc, (int)d);
        return;
    }
    case R_AVR_7_PCREL: {
        long long d = (V - (long long)P - 2) / 2;
        if (d < -64 || d > 63)
            die("%s: a conditional branch reaches +-126 bytes and this "
                "target is %lld away; it has to be an inverted branch "
                "over an rjmp", o->name, (long long)(V - (long long)P));
        avr_patch_br_at(loc, (int)d);
        return;
    }
    default:
        die("%s: unsupported AVR relocation type %u (this is the next "
            "linker increment, not a bug in your program)", o->name, type);
    }
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

/* Xtensa (RELA). R_XTENSA_32 is S + A added to what the field holds
 * (binutils' howto has it partial_inplace; every assembler leaves the
 * field 0). SLOT0_OP names the instruction's one PC-relative operand, so
 * the opcode at the site says which field and which reach: a call's word
 * offset from (P & ~3) + 4, a j's, a branch's or a zero-test's byte
 * offset from P + 4, a loop's forward eight bits to its end (GCC uses the
 * zero-overhead loops on a core that has them), a density beqz.n/bnez.n's
 * forward six bits, an l32r's
 * backward word offset from (P + 3) & ~3. ASM_EXPAND marks a call an
 * assembler could relax, and the DIFF types hold differences between two
 * places in one section, which only a relaxing linker changes: none is
 * an instruction to patch here. */
static void apply_xtensa(struct linker *l, struct object *o, unsigned type,
                         unsigned char *loc, Elf64_Addr S, long long A,
                         Elf64_Addr P)
{
    long long V = (long long)S + A;
    (void)l;
    switch (type) {
    case R_XTENSA_NONE:
    case R_XTENSA_ASM_EXPAND:
    case R_XTENSA_ASM_SIMPLIFY:
    case R_XTENSA_DIFF8:
    case R_XTENSA_DIFF16:
    case R_XTENSA_DIFF32:
        return;
    case R_XTENSA_32:
        put32(loc, (unsigned int)(V + (long long)get32loc(loc)));
        return;
    case R_XTENSA_SLOT0_OP: {
        unsigned long w = (unsigned long)loc[0] | ((unsigned long)loc[1] << 8) |
                          ((unsigned long)loc[2] << 16);
        int op0 = (int)(w & 15), n = (int)(w >> 4) & 3, m = (int)(w >> 6) & 3;
        long long d = V - (long long)P - 4;
        const char *what = NULL;
        if (op0 == 5) {                                   /* callN */
            long long c = V - (long long)((P & ~(Elf64_Addr)3) + 4);
            if (V & 3)
                die("%s: a call to 0x%llx (%s), which is not 4-aligned: an "
                    "Xtensa call's target is a word address", o->name,
                    (unsigned long long)V, l->rel_sym);
            if (c < -524288 || c > 524284)
                die("%s: a call to '%s' is %lld bytes away and call8 "
                    "reaches 512 KiB; this linker mints no trampolines "
                    "(-mlongcalls code calls through a register)",
                    o->name, l->rel_sym, c);
            w = (w & 0x3fUL) | (((unsigned long)(c >> 2) & 0x3ffffUL) << 6);
        } else if (op0 == 6 && n == 0) {                  /* j */
            if (d < -131072 || d > 131071)
                what = "a j reaches 128 KiB";
            else
                w = (w & 0x3fUL) | (((unsigned long)d & 0x3ffffUL) << 6);
        } else if (op0 == 6 && n == 1) {                  /* beqz .. bgez */
            if (d < -2048 || d > 2047)
                what = "a zero-test branch reaches 2 KiB";
            else
                w = (w & 0xfffUL) | (((unsigned long)d & 0xfffUL) << 12);
        } else if (op0 == 7 || (op0 == 6 && (n == 2 || (n == 3 && m >= 2)))) {
            if (d < -128 || d > 127)
                what = "a branch reaches 128 bytes";
            else
                w = (w & 0xffffUL) | (((unsigned long)d & 0xffUL) << 16);
        } else if (op0 == 6 && n == 3 && m == 1 &&
                   ((w >> 12) & 15) >= 8 && ((w >> 12) & 15) <= 10) {
            /* loop, loopnez, loopgtz: the loop's end, 0..255 forward */
            if (d < 0 || d > 255)
                what = "a loop's end is 0..255 bytes past it";
            else
                w = (w & 0xffffUL) | (((unsigned long)d & 0xffUL) << 16);
        } else if (op0 == 12 && (w & 0x80)) {             /* beqz.n bnez.n */
            if (d < 0 || d > 63)
                what = "a beqz.n/bnez.n reaches 63 bytes forward";
            else {
                unsigned long h = (unsigned long)loc[0] | ((unsigned long)loc[1] << 8);
                h = (h & 0x0fcfUL) | (((unsigned long)d & 0x30UL)) |
                    (((unsigned long)d & 0xfUL) << 12);
                loc[0] = (unsigned char)(h & 0xff);
                loc[1] = (unsigned char)(h >> 8);
                return;
            }
        } else if (op0 == 1) {                            /* l32r */
            long long r = V - (long long)((P + 3) & ~(Elf64_Addr)3);
            if (V & 3)
                die("%s: an l32r of a literal at 0x%llx, which is not "
                    "4-aligned", o->name, (unsigned long long)V);
            if (r >= 0 || r < -262144)
                die("%s: an l32r at 0x%llx cannot reach its literal '%s' at "
                    "0x%llx: l32r reaches 256 KiB BACKWARDS, so a literal "
                    "must be placed before the code that loads it (compile "
                    "with -mtext-section-literals)", o->name,
                    (unsigned long long)P, l->rel_sym, (unsigned long long)V);
            w = (w & 0xffUL) | (((unsigned long)(r >> 2) & 0xffffUL) << 8);
        } else {
            die("%s: an R_XTENSA_SLOT0_OP on an instruction this linker "
                "does not decode (0x%06lx at 0x%llx)", o->name, w,
                (unsigned long long)P);
        }
        if (what)
            die("%s: a branch to '%s' at 0x%llx is %lld bytes away and %s; "
                "this linker does not relax", o->name, l->rel_sym,
                (unsigned long long)V, d, what);
        loc[0] = (unsigned char)(w & 0xff);
        loc[1] = (unsigned char)((w >> 8) & 0xff);
        loc[2] = (unsigned char)((w >> 16) & 0xff);
        return;
    }
    default:
        die("%s: unsupported Xtensa relocation type %u against '%s' (EmbLD "
            "applies R_XTENSA_32 and R_XTENSA_SLOT0_OP; PIC, TLS and the "
            "other slots are not supported)", o->name, type, l->rel_sym);
    }
}

static int veneer_needed(int arm, int call, int cond_al, int other,
                         long long d);
static Elf64_Addr arm_veneer_addr(struct linker *l, const struct insec *ts,
                                  struct object *o, Elf64_Word symi,
                                  long long A, int arm);
static int arm_veneer_scan(struct linker *l);
#define VENEER_MAX_ROUNDS 8

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
        int dtgt = o->dbg_sec[target];
        if (o->sec_out[target] < 0 && dtgt < 0)
            continue; /* a non-allocated section nothing kept */
        struct insec *ts = dtgt >= 0 ? (struct insec *)0
                                     : &l->insecs[o->sec_out[target]];
        if (ts && (ts->is_bss || ts->discarded))
            continue;
        /* the output bytes to patch live in the object's own buffer; we
         * patch there, then copy the section into the image at write. */
        /* A DWARF section was already copied into its merged buffer, so
         * that is where its fields are patched; everything else is
         * patched in the object's own buffer and copied at write. */
        unsigned char *base = dtgt >= 0
            ? l->dbgsecs[dtgt].data + o->dbg_off[target]
            : o->buf + sh_at(o, target)->sh_offset;
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
            /* R_RISCV_RELAX carries no symbol -- its index is 0, the
             * null entry -- so it has to be recognised BEFORE the symbol
             * is resolved. It is a hint that the site may be shortened,
             * relaxation is optional, and this linker does not do it.
             * R_RISCV_ALIGN is the same shape: it marks NOP padding that
             * a relaxing linker may shrink, and one that moves nothing
             * leaves the alignment the assembler already arranged. */
            if (o->machine == EM_RISCV &&
                (type == R_RISCV_RELAX || type == R_RISCV_ALIGN))
                continue;
            /* LoongArch's are the same two hints, symbol 0 likewise. */
            if (o->machine == EM_LOONGARCH &&
                (type == R_LARCH_RELAX || type == R_LARCH_ALIGN))
                continue;
            int uw;
            Elf64_Addr S = reloc_symval(l, o, symi, &uw);
            {
                /* /DISCARD/ took the section this symbol is in. DWARF
                 * describing a discarded function gets address 0, as ld
                 * gives it; anything that RUNS cannot point there. */
                Elf64_Sym *dsy = &o->syms[symi];
                const struct insec *dis = NULL;
                if (ELF64_ST_BIND(dsy->st_info) == STB_LOCAL) {
                    if (dsy->st_shndx != SHN_ABS && dsy->st_shndx < (unsigned)o->nsh &&
                        o->sec_out[dsy->st_shndx] >= 0)
                        dis = &l->insecs[o->sec_out[dsy->st_shndx]];
                } else {
                    const char *dn = o->symstr + dsy->st_name;
                    struct symbol *dg = *dn ? sym_find(l, dn) : NULL;
                    if (dg && dg->defined && !dg->scripted && dg->insec >= 0)
                        dis = &l->insecs[dg->insec];
                }
                if (dis && dis->discarded) {
                    if (dtgt >= 0) {
                        S = 0;
                    } else if (dis->gc && ts &&
                               !strcmp(ts->name, ".eh_frame") &&
                               eh_is_fde_pc(ts->data, ts->size, r_offset)) {
                        continue;   /* left as it was: eh_neutralize */
                    } else if (dis->gc) {
                        internal_error("%s: section %s refers to a section "
                                       "--gc-sections removed (%s of %s)",
                                       o->name,
                                       o->shstr + o->shdrs[target].sh_name,
                                       dis->name,
                                       dis->obj ? dis->obj->name : "?");
                    } else {
                        const char *dn = ELF64_ST_TYPE(dsy->st_info) == STT_SECTION
                            ? dis->name : o->symstr + dsy->st_name;
                        die("%s: section %s refers to '%s', which is in section "
                            "%s of %s, and the linker script discards that "
                            "section (/DISCARD/)", o->name,
                            o->shstr + o->shdrs[target].sh_name, dn,
                            dis->name, dis->obj ? dis->obj->name : "?");
                    }
                }
            }
            l->rel_uw = uw;
            {
                Elf64_Sym *sy = &o->syms[symi];
                l->rel_sym = ELF64_ST_TYPE(sy->st_info) == STT_SECTION &&
                             sy->st_shndx < (unsigned)o->nsh
                    ? o->shstr + o->shdrs[sy->st_shndx].sh_name
                    : o->symstr + sy->st_name;
            }
            /* A debug section has no address; every relocation into
             * one is absolute, so there is no P to compute. */
            Elf64_Addr P = ts ? ts->vaddr + r_offset : 0;
            unsigned char *loc = base + r_offset;

            if (o->machine == EM_RISCV) {
                apply_riscv(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_LOONGARCH) {
                apply_loongarch(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_AVR) {
                apply_avr(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_TRICORE) {
                apply_tricore(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_XTENSA) {
                apply_xtensa(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_PPC) {
                if (isrel)
                    die("%s: a PowerPC object with REL relocations; the "
                        "SVR4 ABI's are RELA", o->name);
                apply_ppc(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_RX) {
                apply_rx(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_SPARC) {
                if (isrel)
                    die("%s: a SPARC object with REL relocations; the "
                        "SPARC ABI's are RELA", o->name);
                apply_sparc(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_68K) {
                if (isrel)
                    die("%s: an m68k object with REL relocations; m68k "
                        "objects carry RELA", o->name);
                apply_m68k(l, o, type, loc, S, A, P);
                continue;
            }
            if (o->machine == EM_MIPS) {
                struct mips_relctx mc;
                mc.rbytes = rbytes;
                mc.j = j;
                mc.n = n;
                mc.relsz = relsz;
                mc.isrel = isrel;
                mc.base = base;
                mc.symi = symi;
                mc.local = ELF64_ST_BIND(o->syms[symi].st_info) == STB_LOCAL;
                apply_mips(l, o, type, loc, S, A, P, &mc);
                continue;
            }
            if (o->elf32) {
                /* SHT_REL keeps the addend IN the field, in whatever
                 * shape that field has -- a word for the data
                 * relocations, a branch displacement for a call, a
                 * 16-bit immediate split across four fields for a
                 * movw. Each is read back the way it was written. */
                if (isrel) {
                    switch (type) {
                    case R_ARM_ABS32:
                    case R_ARM_TARGET1:
                    case R_ARM_REL32:
                        A = (int)get32loc(loc);
                        break;
                    case R_ARM_PREL31:
                        /* 31 bits, signed; bit 31 is not the addend's */
                        A = (int)(get32loc(loc) << 1) >> 1;
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
                    case R_ARM_CALL:
                    case R_ARM_JUMP24:
                        /* from the instruction + 8, as THM_CALL's is +4:
                         * an assembler's `bl f` holds -8 */
                        A = read_arm_b24(loc) + 8;
                        break;
                    case R_ARM_MOVW_ABS_NC:
                    case R_ARM_MOVT_ABS:
                        A = (short)read_arm_mov(loc);
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
                case R_ARM_NONE:
                    break;
                case R_ARM_TARGET1:
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
                case R_ARM_THM_JUMP24: {
                    /* A call from Thumb code to an ARM-state function --
                     * a global function symbol with the Thumb bit CLEAR,
                     * which only an A32 object (armv7a) defines -- is
                     * `blx`: the second halfword's bit 12 clear, and the
                     * displacement from Align(P + 4, 4) to the word-aligned
                     * target. A jump there, and any branch out of reach,
                     * goes to its veneer (arm_veneers): Thumb code within
                     * reach, which goes the rest of the way. */
                    int to_arm = thumb_to_arm(l, o, symi, S);
                    long long d = (long long)(S & ~(Elf64_Addr)1) + A -
                                  ((long long)P + 4);
                    if (veneer_needed(0, type == R_ARM_THM_CALL, 1, to_arm,
                                      d)) {
                        Elf64_Addr vn = arm_veneer_addr(l, ts, o, symi, A, 0);
                        if (vn) {
                            S = vn;
                            A = 0;
                            to_arm = 0;
                        }
                    }
                    if (to_arm) {
                        if (type != R_ARM_THM_CALL)
                            internal_error("%s: the Thumb branch to the "
                                           "ARM-state function '%s' has no "
                                           "veneer", o->name, l->rel_sym);
                        patch_thm_b24(o, loc, (long long)S + A -
                                      (long long)((P + 4) & ~(Elf64_Addr)3));
                        put16(loc + 2, get16(loc + 2) & ~0x1000u);
                        break;
                    }
                    /* The displacement is between ADDRESSES, so the
                     * Thumb bit comes off S first -- leaving it on would
                     * shift every call by one byte. */
                    patch_thm_b24(o, loc,
                                  (long long)(S & ~(Elf64_Addr)1) + A -
                                  ((long long)P + 4));
                    break;
                }
                case R_ARM_THM_MOVW_ABS_NC:
                    patch_thm_mov(loc, (unsigned int)((S + (Elf64_Addr)A)
                                                      & 0xffff));
                    break;
                case R_ARM_CALL:
                case R_ARM_JUMP24: {
                    /* the Thumb bit comes off for the displacement, and
                     * says the call must switch state (blx); a branch out
                     * of reach, or a jump or a conditional call that must
                     * switch state, goes to its veneer */
                    int thumb = (int)(S & 1);
                    long long d = (long long)(S & ~(Elf64_Addr)1) + A -
                                  ((long long)P + 8);
                    if (veneer_needed(1, type == R_ARM_CALL,
                                      (get32loc(loc) >> 28) == 0xe, thumb,
                                      d)) {
                        Elf64_Addr vn = arm_veneer_addr(l, ts, o, symi, A, 1);
                        if (vn) {
                            S = vn;
                            A = 0;
                            thumb = 0;
                        }
                    }
                    patch_arm_b24(o, loc,
                                  (long long)(S & ~(Elf64_Addr)1) + A -
                                  ((long long)P + 8),
                                  type == R_ARM_CALL, thumb);
                    break;
                }
                case R_ARM_MOVW_ABS_NC:
                    patch_arm_mov(loc, (unsigned int)((S + (Elf64_Addr)A)
                                                      & 0xffff));
                    break;
                case R_ARM_MOVT_ABS:
                    patch_arm_mov(loc, (unsigned int)(((S + (Elf64_Addr)A)
                                                       >> 16) & 0xffff));
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
                need_range(l, o, "R_X86_64_32", (long long)S + A, 0,
                           0xffffffffLL);
                put32(loc, (unsigned int)(S + A));
                break;
            case R_X86_64_32S:
                need_range(l, o, "R_X86_64_32S", (long long)S + A,
                           I32_MIN, I32_MAX);
                put32(loc, (unsigned int)(S + A));
                break;
            case R_X86_64_PC32:
            case R_X86_64_PLT32:
                /* TARGET_ABI §4a: PLT32 is a plain PC32 in a static
                 * link — no PLT slot is minted. */
                need_range(l, o, type == R_X86_64_PC32 ? "R_X86_64_PC32"
                                                       : "R_X86_64_PLT32",
                           (long long)S + A - (long long)P, I32_MIN, I32_MAX);
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
                need_range(l, o, "R_X86_64_TPOFF32",
                           (long long)S + A - (long long)l->tls_start -
                           (long long)l->tls_size, I32_MIN, I32_MAX);
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

/* A string table for the executable's own .strtab/.shstrtab. Small and
 * local: the object writer has its own, and threading that one through
 * the linker would couple two files that otherwise share nothing. */
struct ltab { char *p; long len, cap; };

static long ltab_add(struct ltab *t, const char *s)
{
    long n = (long)strlen(s) + 1, at;
    if (!t->p) {                       /* index 0 is always the empty name */
        t->cap = 256;
        t->p = xmalloc((size_t)t->cap);
        t->p[0] = 0;
        t->len = 1;
    }
    if (!*s) return 0;
    while (t->len + n > t->cap) {
        t->cap *= 2;
        t->p = xrealloc(t->p, (size_t)t->cap);
    }
    at = t->len;
    memcpy(t->p + at, s, (size_t)n);
    t->len += n;
    return at;
}

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
     * needs to change for it.
     *
     * Not a FIRMWARE image (-Tdata): nothing hands it AT_PHDR -- it is
     * copied into flash and its startup finds the template by the
     * linker's symbols -- and covering the headers started its text
     * segment that many bytes below the text, which at -Ttext 0 is
     * 0xffffff6c: QEMU loaded no vector table and the Cortex-M locked
     * up at reset, for any program with a thread_local in it. */
    int ntls = tls_memsz ? 1 : 0;
    int hdrs_in_text = ntls && !l->data_base;
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

    /* ---- a SYMBOL TABLE in the executable -----------------------------
     *
     * Not loaded -- the section headers and the two string tables sit
     * outside every PT_LOAD, so a firmware image copied to flash is the
     * segments and none of this. It costs file size and no bytes on the
     * board, which is why every linker keeps it unless asked not to.
     *
     * It is here because without it a linked image is anonymous: EmbDBG
     * could symbolize a .o and not the firmware built from it, so
     * `embdbg fw.elf remote :1234` had nothing to say about where the
     * target had stopped. llvm-objdump gains the same names.
     */
    struct { const char *name; Elf64_Addr val; Elf64_Xword size;
             int text; int type; } *sy;
    int nsy = 0, nloc = 0, cap = l->nsym + 1;
    for (int i = 0; i < l->nobj; i++)
        cap += l->objs[i]->local_syms;
    sy = xmalloc((size_t)cap * sizeof *sy);
    /* The objects' LOCAL functions and objects too, first, as ELF wants
     * every local ahead of the first global (sh_info says where that
     * is). A static function is most of a firmware image, and without
     * its name a debugger stopped inside one reports the global before
     * it plus an offset, and a profile charges its time to that global.
     * Each is where its section landed, as a relocation against it
     * would resolve; one whose section was not laid out is dropped. */
    for (int i = 0; i < l->nobj; i++) {
        struct object *o = l->objs[i];
        for (int k = 1; k < o->local_syms && k < o->nsym; k++) {
            Elf64_Sym *s = &o->syms[k];
            int t = ELF64_ST_TYPE(s->st_info), out;
            const char *name = o->symstr + s->st_name;
            if ((t != STT_FUNC && t != STT_OBJECT) || !*name ||
                s->st_shndx == SHN_UNDEF || s->st_shndx >= o->nsh)
                continue;                               /* also ABS, COMMON */
            out = o->sec_out[s->st_shndx];
            if (out < 0 || l->insecs[out].discarded)
                continue;
            sy[nsy].name = name;
            sy[nsy].val = l->insecs[out].vaddr + s->st_value;
            sy[nsy].size = s->st_size;
            sy[nsy].text = l->insecs[out].seg == SEG_TEXT;
            sy[nsy].type = t;
            nsy++;
        }
    }
    nloc = nsy;
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *sm = &l->syms[i];
        if (!sym_in_image(l, sm))
            continue;
        sy[nsy].name = sm->name;
        sy[nsy].val = sm->value;
        sy[nsy].size = sm->size;
        /* STT_FUNC for anything defined in the text segment: it is what
         * a debugger filters on, and a symbol with no size is skipped
         * there, so the size has to travel too. */
        sy[nsy].text = sm->insec >= 0 && l->insecs[sm->insec].seg == SEG_TEXT;
        sy[nsy].type = sm->type ? sm->type
                     : (sy[nsy].text ? STT_FUNC : STT_OBJECT);
        nsy++;
    }

    int have_data = data_filesz > 0;
    /* [0] NULL  [1] .text  ([2] .data)  .symtab  .strtab  .shstrtab */
    int sh_text = 1, sh_data = have_data ? 2 : 0;
    int sh_symtab = have_data ? 3 : 2;
    /* The merged .debug_* sections go between .text/.data and the
     * symbol table: they are not allocated, so they take no address
     * and sit only in the file. */
    int sh_dbg0 = sh_symtab;
    sh_symtab += l->ndbg;
    int sh_strtab = sh_symtab + 1, sh_shstr = sh_symtab + 2;
    int nsh = sh_shstr + 1;

    struct ltab symstr, shstr;
    memset(&symstr, 0, sizeof symstr);
    memset(&shstr, 0, sizeof shstr);
    ltab_add(&symstr, "");
    ltab_add(&shstr, "");
    Elf64_Word *symname = xmalloc((size_t)(nsy + 1) * sizeof *symname);
    for (int i = 0; i < nsy; i++)
        symname[i] = (Elf64_Word)ltab_add(&symstr, sy[i].name);
    Elf64_Word n_text = (Elf64_Word)ltab_add(&shstr, ".text");
    Elf64_Word n_data = have_data ? (Elf64_Word)ltab_add(&shstr, ".data") : 0;
    Elf64_Word n_symtab = (Elf64_Word)ltab_add(&shstr, ".symtab");
    Elf64_Word n_strtab = (Elf64_Word)ltab_add(&shstr, ".strtab");
    Elf64_Word n_shstr = (Elf64_Word)ltab_add(&shstr, ".shstrtab");
    Elf64_Word *n_dbg = l->ndbg
        ? xmalloc((size_t)l->ndbg * sizeof *n_dbg) : (Elf64_Word *)0;
    for (int i = 0; i < l->ndbg; i++)
        n_dbg[i] = (Elf64_Word)ltab_add(&shstr, l->dbgsecs[i].name);

    Elf64_Xword symentsz = l->elf32 ? sizeof(Elf32_Sym) : sizeof(Elf64_Sym);
    Elf64_Xword shentsz  = l->elf32 ? sizeof(Elf32_Shdr) : sizeof(Elf64_Shdr);

    Elf64_Off dbg_off0 = align_up(data_off + data_filesz, 8);
    Elf64_Off dbg_total = 0;
    Elf64_Off *dbg_at = l->ndbg
        ? xmalloc((size_t)l->ndbg * sizeof *dbg_at) : (Elf64_Off *)0;
    for (int i = 0; i < l->ndbg; i++) {
        dbg_at[i] = dbg_off0 + dbg_total;
        dbg_total += (Elf64_Off)l->dbgsecs[i].len;
    }
    Elf64_Off sym_off = align_up(dbg_off0 + dbg_total, 8);
    Elf64_Off symsz = (Elf64_Xword)(nsy + 1) * symentsz;   /* +1: the null */
    Elf64_Off str_off = sym_off + symsz;
    Elf64_Off shstr_off = str_off + symstr.len;
    Elf64_Off sh_off = align_up(shstr_off + shstr.len, 8);

    Elf64_Off total = sh_off + (Elf64_Off)nsh * shentsz;
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
    /* RISC-V's and AVR's from the inputs (see l->eflags); bits 2:1 of
     * RISC-V's are the float ABI, whose 0 means SOFT. */
    eh->e_flags = l->machine == EM_ARM ? EF_ARM_EABI_VER5
                : l->machine == EM_RISCV || l->machine == EM_AVR ||
                  l->machine == EM_MIPS || l->machine == EM_LOONGARCH ||
                  l->machine == EM_TRICORE || l->machine == EM_RX ||
                  l->machine == EM_68K
                ? l->eflags
                : 0;
    eh->e_phoff = ehsz;
    eh->e_ehsize = (Elf64_Half)ehsz;
    eh->e_phentsize = (Elf64_Half)phsz;
    eh->e_phnum = (Elf64_Half)nph;

    Elf64_Phdr *ph = phbuf;
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R | PF_X;
    if (hdrs_in_text) {
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
        if (s->is_bss || !s->data || s->discarded)
            continue;
        Elf64_Off base = (s->seg == SEG_TEXT) ? text_off : data_off;
        Elf64_Addr segva = (s->seg == SEG_TEXT) ? text_start : data_start;
        memcpy(img + base + (s->vaddr - segva), s->data, (size_t)s->size);
    }

    /* ---- the symbol table and the section headers --------------------- */
    memcpy(img + str_off, symstr.p, (size_t)symstr.len);
    memcpy(img + shstr_off, shstr.p, (size_t)shstr.len);
    for (int i = 0; i < nsy; i++) {
        /* Index 0 is the reserved null entry, already zeroed. */
        Elf64_Half shndx = (Elf64_Half)(sy[i].text ? sh_text
                                        : (have_data ? sh_data : sh_text));
        unsigned char info = (unsigned char)ELF64_ST_INFO(
            i < nloc ? STB_LOCAL : STB_GLOBAL, sy[i].type);
        if (l->elf32) {
            Elf32_Sym e;
            memset(&e, 0, sizeof e);
            e.st_name = symname[i];
            e.st_value = (Elf32_Addr)sy[i].val;
            e.st_size = (Elf32_Word)sy[i].size;
            e.st_info = info;
            e.st_shndx = shndx;
            memcpy(img + sym_off + (size_t)(i + 1) * symentsz, &e, sizeof e);
        } else {
            Elf64_Sym e;
            memset(&e, 0, sizeof e);
            e.st_name = symname[i];
            e.st_value = sy[i].val;
            e.st_size = sy[i].size;
            e.st_info = info;
            e.st_shndx = shndx;
            memcpy(img + sym_off + (size_t)(i + 1) * symentsz, &e, sizeof e);
        }
    }
    {
        /* Built as the 64-bit shape and narrowed at the one place that
         * serialises it, exactly as the headers above are. */
        /* nsh entries, not a fixed six: the merged DWARF sections
         * added as many headers as there are distinct .debug_* names,
         * and writing past a six-element array is how that first
         * showed up -- as an image with no debug sections in it. */
        Elf64_Shdr *sh = xcalloc((size_t)nsh, sizeof *sh);
        sh[sh_text].sh_name = n_text;
        sh[sh_text].sh_type = SHT_PROGBITS;
        sh[sh_text].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
        sh[sh_text].sh_addr = text_start;
        sh[sh_text].sh_offset = text_off;
        sh[sh_text].sh_size = text_size;
        sh[sh_text].sh_addralign = 4;
        if (have_data) {
            sh[sh_data].sh_name = n_data;
            sh[sh_data].sh_type = SHT_PROGBITS;
            sh[sh_data].sh_flags = SHF_ALLOC | SHF_WRITE;
            sh[sh_data].sh_addr = data_start;
            sh[sh_data].sh_offset = data_off;
            sh[sh_data].sh_size = data_filesz;
            sh[sh_data].sh_addralign = 4;
        }
        /* The merged DWARF. No SHF_ALLOC and no address: it is in the
         * file for a debugger and nowhere in the running image. */
        for (int i = 0; i < l->ndbg; i++) {
            sh[sh_dbg0 + i].sh_name = n_dbg[i];
            sh[sh_dbg0 + i].sh_type = SHT_PROGBITS;
            sh[sh_dbg0 + i].sh_flags = 0;
            sh[sh_dbg0 + i].sh_offset = dbg_at[i];
            sh[sh_dbg0 + i].sh_size = (Elf64_Xword)l->dbgsecs[i].len;
            sh[sh_dbg0 + i].sh_addralign = 1;
            memcpy(img + dbg_at[i], l->dbgsecs[i].data,
                   (size_t)l->dbgsecs[i].len);
        }
        sh[sh_symtab].sh_name = n_symtab;
        sh[sh_symtab].sh_type = SHT_SYMTAB;
        sh[sh_symtab].sh_offset = sym_off;
        sh[sh_symtab].sh_size = symsz;
        sh[sh_symtab].sh_link = (Elf64_Word)sh_strtab;
        sh[sh_symtab].sh_info = (Elf64_Word)(1 + nloc);  /* the null entry
                                                          * and the locals */
        sh[sh_symtab].sh_addralign = 8;
        sh[sh_symtab].sh_entsize = symentsz;
        sh[sh_strtab].sh_name = n_strtab;
        sh[sh_strtab].sh_type = SHT_STRTAB;
        sh[sh_strtab].sh_offset = str_off;
        sh[sh_strtab].sh_size = (Elf64_Xword)symstr.len;
        sh[sh_strtab].sh_addralign = 1;
        sh[sh_shstr].sh_name = n_shstr;
        sh[sh_shstr].sh_type = SHT_STRTAB;
        sh[sh_shstr].sh_offset = shstr_off;
        sh[sh_shstr].sh_size = (Elf64_Xword)shstr.len;
        sh[sh_shstr].sh_addralign = 1;
        for (int i = 0; i < nsh; i++) {
            if (l->elf32) {
                Elf32_Shdr s32;
                memset(&s32, 0, sizeof s32);
                s32.sh_name = sh[i].sh_name;
                s32.sh_type = sh[i].sh_type;
                s32.sh_flags = (Elf32_Word)sh[i].sh_flags;
                s32.sh_addr = (Elf32_Addr)sh[i].sh_addr;
                s32.sh_offset = (Elf32_Off)sh[i].sh_offset;
                s32.sh_size = (Elf32_Word)sh[i].sh_size;
                s32.sh_link = sh[i].sh_link;
                s32.sh_info = sh[i].sh_info;
                s32.sh_addralign = (Elf32_Word)sh[i].sh_addralign;
                s32.sh_entsize = (Elf32_Word)sh[i].sh_entsize;
                memcpy(img + sh_off + (size_t)i * shentsz, &s32, sizeof s32);
            } else {
                memcpy(img + sh_off + (size_t)i * shentsz, &sh[i], sizeof sh[i]);
            }
        }
        free(sh);
    }
    if (l->elf32) {
        Elf32_Ehdr *e = (Elf32_Ehdr *)img;
        e->e_shoff = (Elf32_Off)sh_off;
        e->e_shentsize = (Elf32_Half)shentsz;
        e->e_shnum = (Elf32_Half)nsh;
        e->e_shstrndx = (Elf32_Half)sh_shstr;
    } else {
        Elf64_Ehdr *e = (Elf64_Ehdr *)img;
        e->e_shoff = sh_off;
        e->e_shentsize = (Elf64_Half)shentsz;
        e->e_shnum = (Elf64_Half)nsh;
        e->e_shstrndx = (Elf64_Half)sh_shstr;
    }
    free(sy); free(symname); free(symstr.p); free(shstr.p);

    if (l->big_endian) {
        if (l->elf32) be_image(img);
        else          be_image64(img);
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
    if (l->big_endian)
        die("--embx: an EMBX image is little-endian x86-64's (EmbLinkOS); "
            "it is not written for a big-endian link");
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
        if (s->is_bss || !s->data || s->discarded) continue;
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

    if (n && l->big_endian) {
        /* its reader (tools/embdbg) parses little-endian ELF and DWARF;
         * the image itself carries the DWARF a debugger reads */
        fprintf(stderr, "embld: no .embdbg sidecar for a big-endian image "
                "(EmbDBG reads little-endian objects only)\n");
        n = 0;
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

/* ---- linker scripts: the layout ----------------------------------------
 *
 * ldscript.h parsed the script into a list of commands; this lays the
 * image out by them, the way GNU ld does:
 *
 *   - Each input section is claimed by the FIRST input description that
 *     matches it, in script order; a claimed section is not matched again.
 *     Within one description, inputs go in command-line order and each
 *     file's sections in their own order, unless a SORT says otherwise.
 *   - An allocated input nothing claims is an ORPHAN, and gets an output
 *     section of its own name after the last one of its kind (code,
 *     read-only data, data, zero-initialised).
 *   - An output section starts at its address if it has one, else at its
 *     region's next free byte if it names one (`> RAM`), else at `.`. Its
 *     load address is AT(), or the next free byte of its AT> region, or
 *     its run address shifted the way the previous section in the same
 *     region was, or its run address. One `cur` per region serves both
 *     uses, which is how `.data`'s initial bytes land right after the
 *     code in flash.
 *   - Symbol assignments are evaluated where they stand, so `_sdata = .`
 *     inside .data is .data's address there. A forward reference
 *     (`LOADADDR(.data)` above .data) reads the previous pass; the layout
 *     is repeated until nothing moves.
 */

#define LS_MAX_PASSES 8

struct ls_state {
    struct linker *l;
    struct ls_script *sc;
    long long dot;
    int in_sections;            /* `.` exists */
    int cur;                    /* the output section being laid, or -1 */
    int final;                  /* errors are reported only in the last pass */
    const char *file;           /* for messages: the statement's place */
    int line;
};

static void ls_die(struct ls_state *st, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    die("%s:%d: %s", st->file ? st->file : "<script>", st->line, msg);
}

static int ls_region_idx(const struct ls_script *sc, const char *name)
{
    for (int k = 0; k < sc->nalias; k++)
        if (!strcmp(sc->alias[k].alias, name)) {
            name = sc->alias[k].region;
            break;
        }
    for (int k = 0; k < sc->nreg; k++)
        if (!strcmp(sc->reg[k].name, name))
            return k;
    return -1;
}

static int ls_osec_idx(const struct ls_script *sc, const char *name)
{
    for (int k = 0; k < sc->nosec; k++)
        if (!sc->osecs[k].discard && !strcmp(sc->osecs[k].name, name))
            return k;
    return -1;
}

/* A symbol's address as it stands mid-layout: an object's symbol is its
 * section's address so far plus its offset (finalize_symbols has not run
 * yet), a script's or a COMMON's is already absolute. */
static int ls_symval(struct linker *l, const char *name, long long *v,
                     int *rel)
{
    struct symbol *g = sym_find(l, name);
    if (!g || !g->defined)
        return 0;
    if (g->scripted || g->common || g->insec < 0) {
        *v = (long long)g->value;
        *rel = g->scripted ? g->sosec >= 0 : g->insec >= 0 || g->common;
        return 1;
    }
    *v = (long long)(l->insecs[g->insec].vaddr + g->value);
    *rel = 1;
    return 1;
}

static long long ls_align_up(struct ls_state *st, long long v, long long a)
{
    if (a <= 1)
        return v;
    if (a & (a - 1))
        ls_die(st, "an alignment of %lld is not a power of two", a);
    return (v + a - 1) & ~(a - 1);
}

static long long ls_eval(struct ls_state *st, const struct lx *e, int *rel);

static long long ls_eval_abs(struct ls_state *st, const struct lx *e)
{
    int rel;
    return ls_eval(st, e, &rel);
}

static long long ls_eval(struct ls_state *st, const struct lx *e, int *rel)
{
    struct ls_script *sc = st->sc;
    long long a, b;
    int ra = 0, rb = 0;
    *rel = 0;
    switch (e->kind) {
    case LX_NUM:
        return e->num;
    case LX_DOT:
        if (!st->in_sections)
            ls_die(st, "'.' is the location counter, which exists only "
                   "inside SECTIONS");
        *rel = 1;
        return st->dot;
    case LX_SYM: {
        long long v;
        if (ls_symval(st->l, e->name, &v, rel))
            return v;
        if (st->final)
            ls_die(st, "the script uses '%s', which nothing defines", e->name);
        return 0;
    }
    case LX_UN:
        a = ls_eval(st, e->a, &ra);
        return e->op == LO_NEG ? -a : e->op == LO_NOT ? !a : ~a;
    case LX_COND:
        a = ls_eval(st, e->a, &ra);
        return ls_eval(st, a ? e->b : e->c, rel);
    case LX_BIN:
        if (e->op == LO_LAND || e->op == LO_LOR) {
            a = ls_eval(st, e->a, &ra);
            if (e->op == LO_LAND ? !a : a)
                return e->op == LO_LOR;
            return ls_eval(st, e->b, &rb) != 0;
        }
        a = ls_eval(st, e->a, &ra);
        b = ls_eval(st, e->b, &rb);
        switch (e->op) {
        case LO_ADD: *rel = ra || rb; return a + b;
        case LO_SUB: *rel = ra && !rb; return a - b;
        case LO_MUL: return a * b;
        case LO_DIV:
        case LO_MOD:
            if (!b)
                ls_die(st, "division by zero");
            return e->op == LO_DIV ? a / b : a % b;
        case LO_SHL: return (long long)((unsigned long long)a << (b & 63));
        case LO_SHR: return (long long)((unsigned long long)a >> (b & 63));
        case LO_AND: return a & b;
        case LO_OR:  return a | b;
        case LO_XOR: return a ^ b;
        case LO_EQ:  return a == b;
        case LO_NE:  return a != b;
        case LO_LT:  return a < b;
        case LO_LE:  return a <= b;
        case LO_GT:  return a > b;
        default:     return a >= b;
        }
    case LX_FN:
        break;
    default:
        ls_die(st, "a malformed expression");
    }
    switch (e->op) {
    case LF_ALIGN:
        if (!st->in_sections)
            ls_die(st, "ALIGN(n) aligns '.', which exists only inside "
                   "SECTIONS; ALIGN(expr, n) aligns a value");
        *rel = 1;
        return ls_align_up(st, st->dot, ls_eval_abs(st, e->a));
    case LF_ALIGN2:
        a = ls_eval(st, e->a, rel);
        return ls_align_up(st, a, ls_eval_abs(st, e->b));
    case LF_ORIGIN:
    case LF_LENGTH: {
        int r = ls_region_idx(sc, e->name);
        if (r < 0)
            ls_die(st, "there is no memory region %s", e->name);
        return e->op == LF_ORIGIN ? sc->reg[r].origin : sc->reg[r].length;
    }
    case LF_ADDR: case LF_LOADADDR: case LF_SIZEOF: case LF_ALIGNOF: {
        int k = ls_osec_idx(sc, e->name);
        if (k < 0)
            ls_die(st, "there is no output section %s", e->name);
        const struct ls_osec *o = &sc->osecs[k];
        if (e->op == LF_ADDR) {
            *rel = 1;
            return o->vma;
        }
        return e->op == LF_LOADADDR ? o->lma
             : e->op == LF_SIZEOF ? o->size : o->al;
    }
    case LF_DEFINED: {
        struct symbol *g = sym_find(st->l, e->name);
        return g && g->defined;
    }
    case LF_MAX:
    case LF_MIN:
        a = ls_eval(st, e->a, &ra);
        b = ls_eval(st, e->b, &rb);
        if ((e->op == LF_MAX) == (a >= b)) {
            *rel = ra;
            return a;
        }
        *rel = rb;
        return b;
    case LF_ABSOLUTE:
        return ls_eval_abs(st, e->a);
    case LF_LOG2CEIL: {
        unsigned long long v = (unsigned long long)ls_eval_abs(st, e->a);
        int k = 0;
        while (k < 64 && (1ULL << k) < v)
            k++;
        return k;
    }
    case LF_CONSTANT:
        if (!strcmp(e->name, "MAXPAGESIZE") ||
            !strcmp(e->name, "COMMONPAGESIZE"))
            return 0x1000;
        ls_die(st, "CONSTANT(%s) is not a constant embld knows", e->name);
        return 0;
    default:                        /* SIZEOF_HEADERS: none are loaded */
        return 0;
    }
}

static void ls_assign(struct ls_state *st, const struct ls_stmt *s)
{
    struct linker *l = st->l;
    int rel;
    long long v = ls_eval(st, s->e, &rel);
    st->file = s->file;
    st->line = s->line;
    if (!strcmp(s->sym, ".")) {
        if (!st->in_sections)
            ls_die(st, "'.' can be assigned only inside SECTIONS");
        if (s->aop) {
            long long old = st->dot;
            rel = 1;
            v = s->aop == LO_ADD ? old + v : s->aop == LO_SUB ? old - v
              : s->aop == LO_MUL ? old * v : s->aop == LO_DIV && v ? old / v
              : s->aop == LO_AND ? (old & v) : s->aop == LO_OR ? (old | v)
              : s->aop == LO_SHL ? (long long)((unsigned long long)old << (v & 63))
              : (long long)((unsigned long long)old >> (v & 63));
        }
        if (st->cur >= 0) {
            /* inside an output section an absolute value is an offset
             * into it: `. = 0x100;` is 0x100 bytes in, as ld has it */
            if (!rel)
                v += st->sc->osecs[st->cur].vma;
            if (v < st->dot)
                ls_die(st, "the location counter cannot move backwards "
                       "inside %s (from 0x%llx to 0x%llx)",
                       st->sc->osecs[st->cur].name,
                       (unsigned long long)st->dot, (unsigned long long)v);
        }
        st->dot = v;
        return;
    }
    struct symbol *g = sym_intern(l, s->sym);
    if (s->aop) {
        long long old;
        int r2;
        if (!ls_symval(l, s->sym, &old, &r2)) {
            if (st->final)
                ls_die(st, "a compound assignment to '%s', which is not "
                       "defined", s->sym);
            old = 0;
        }
        v = s->aop == LO_ADD ? old + v : s->aop == LO_SUB ? old - v
          : s->aop == LO_MUL ? old * v : s->aop == LO_DIV && v ? old / v
          : s->aop == LO_AND ? (old & v) : s->aop == LO_OR ? (old | v)
          : s->aop == LO_SHL ? (long long)((unsigned long long)old << (v & 63))
          : (long long)((unsigned long long)old >> (v & 63));
    }
    /* PROVIDE: only when no object defines it. A plain assignment wins
     * over an object's definition, as it does in ld. */
    if (s->provide && g->defined && !g->scripted)
        return;
    g->defined = 1;
    g->weak = 0;
    g->common = 0;
    g->obj = NULL;
    g->insec = -1;
    g->scripted = 1;
    g->value = (Elf64_Addr)v;
    g->sosec = rel ? (st->cur >= 0 ? st->cur : -2) : -1;
}

static int ls_class(Elf64_Xword flags, int bss)
{
    if (flags & SHF_EXECINSTR) return 0;          /* code */
    if (bss) return 3;                            /* zero-initialised */
    if (flags & SHF_WRITE) return 2;              /* data */
    return 1;                                     /* read-only data */
}

static struct linker *g_ls_sort_l;
static int g_ls_sort_kind;

/* `.init_array.00101`'s priority, 101; 65536 for a name without one. */
static unsigned long ls_init_priority(const char *name)
{
    const char *d = strrchr(name, '.');
    if (!d || !d[1])
        return 65536;               /* no priority: after every numbered one */
    for (const char *q = d + 1; *q; q++)
        if (*q < '0' || *q > '9')
            return 65536;
    return strtoul(d + 1, NULL, 10);
}

static int ls_sort_cmp(const void *pa, const void *pb)
{
    const struct insec *a = &g_ls_sort_l->insecs[*(const int *)pa];
    const struct insec *b = &g_ls_sort_l->insecs[*(const int *)pb];
    int c = 0;
    if (g_ls_sort_kind == LSORT_NAME) {
        c = strcmp(a->name, b->name);
    } else if (g_ls_sort_kind == LSORT_ALIGN) {
        c = a->align > b->align ? -1 : a->align < b->align;
    } else {
        unsigned long pa2 = ls_init_priority(a->name);
        unsigned long pb2 = ls_init_priority(b->name);
        c = pa2 < pb2 ? -1 : pa2 > pb2;
    }
    /* stable: fall back on the order they were claimed in */
    return c ? c : (*(const int *)pa > *(const int *)pb) -
                   (*(const int *)pa < *(const int *)pb);
}

static void ls_push_list(struct ls_stmt *s, int v)
{
    LS_PUSH(s->list, s->nlist, s->caplist);
    s->list[s->nlist++] = v;
}

/* Every input description claims what it matches, once, in order. */
static void ls_claim(struct linker *l, struct ls_script *sc)
{
    for (int k = 0; k < sc->nosec; k++) {
        struct ls_osec *o = &sc->osecs[k];
        for (int b = 0; b < o->nbody; b++) {
            struct ls_stmt *s = &o->body[b];
            int sort = 0;
            if (s->kind != LS_INPUT)
                continue;
            for (int ob = 0; ob < l->nobj; ob++) {
                struct object *obj = l->objs[ob];
                if (!ls_file_match(s->in.file, obj->name) ||
                    ls_file_excluded(s->in.fexcl, s->in.nfexcl, obj->name))
                    continue;
                for (int i = 0; i < obj->nsh; i++) {
                    int si = obj->sec_out[i];
                    if (si < 0 || l->insecs[si].sosec != -1)
                        continue;
                    struct insec *is = &l->insecs[si];
                    for (int j = 0; j < s->in.nsec; j++) {
                        if (!strcmp(s->in.sec[j], "COMMON") ||
                            ls_file_excluded(s->in.sexcl[j], s->in.nsexcl[j],
                                             obj->name) ||
                            !ls_glob(s->in.sec[j], is->name))
                            continue;
                        is->sosec = o->discard ? -2 : k;
                        is->discarded = o->discard;
                        is->keep = s->in.keep;
                        if (s->in.sort[j])
                            sort = s->in.sort[j];
                        ls_push_list(s, si);
                        break;
                    }
                }
            }
            /* the linker's own sections (the CMSE veneers), which belong
             * to no file: matched by name under a `*` file pattern */
            for (int si = 0; si < l->nsec; si++) {
                struct insec *is = &l->insecs[si];
                if (is->obj || is->sosec != -1 ||
                    !ls_file_match(s->in.file, "*linker stubs*"))
                    continue;
                for (int j = 0; j < s->in.nsec; j++) {
                    if (strcmp(s->in.sec[j], "COMMON") &&
                        ls_glob(s->in.sec[j], is->name)) {
                        is->sosec = o->discard ? -2 : k;
                        is->discarded = o->discard;
                        is->keep = 1;
                        ls_push_list(s, si);
                        break;
                    }
                }
            }
            if (sort && s->nlist > 1) {
                g_ls_sort_l = l;
                g_ls_sort_kind = sort;
                qsort(s->list, (size_t)s->nlist, sizeof *s->list, ls_sort_cmp);
            }
        }
    }
}

/* What kind of output section a script one is, from what it claimed: the
 * orphan rule needs it. -1 when it claimed nothing. */
static int ls_osec_class(struct linker *l, const struct ls_osec *o)
{
    int cls = -1;
    for (int b = 0; b < o->nbody; b++) {
        const struct ls_stmt *s = &o->body[b];
        for (int k = 0; k < s->nlist; k++) {
            const struct insec *is = &l->insecs[s->list[k]];
            int c = ls_class(is->shflags, is->is_bss);
            if (cls < 0 || c < cls || (cls == 3 && c != 3))
                cls = c;
        }
        if (s->kind == LS_INPUT && s->in.common && cls < 0)
            cls = 3;
    }
    return cls;
}

/* Unclaimed allocated inputs: an output section per name, placed after
 * the last script section of the same kind, as ld places orphans. */
static void ls_orphans(struct linker *l, struct ls_script *sc, int mode)
{
    for (int si = 0; si < l->nsec; si++) {
        struct insec *is = &l->insecs[si];
        if (is->sosec != -1 || is->discarded)
            continue;
        if (is->osec == OSEC_TDATA || is->osec == OSEC_TBSS)
            die("%s: thread-local section %s in a linker-script link is "
                "not supported", is->obj ? is->obj->name : "?", is->name);
        int k = -1;
        for (int j = 0; j < sc->nosec; j++)
            if (sc->osecs[j].orphan && !strcmp(sc->osecs[j].name, is->name))
                k = j;
        if (k < 0) {
            if (mode == 2)
                die("%s: section %s is placed by no rule of the linker "
                    "script (--orphan-handling=error)",
                    is->obj ? is->obj->name : "?", is->name);
            if (mode == 1)
                fprintf(stderr, "embld: warning: %s: section %s is placed by "
                        "no rule of the linker script; it gets an output "
                        "section of its own\n",
                        is->obj ? is->obj->name : "?", is->name);
            int cls = ls_class(is->shflags, is->is_bss);
            /* after the last LS_OSEC command of this kind (or of the
             * nearest kind before it), and after orphans already put
             * there */
            int at = -1;
            for (int want = cls; want >= 0 && at < 0; want--)
                for (int c = 0; c < sc->ncmd; c++) {
                    if (sc->cmds[c].kind != LS_OSEC)
                        continue;
                    struct ls_osec *o = &sc->osecs[sc->cmds[c].osec];
                    if (o->discard)
                        continue;
                    if (ls_osec_class(l, o) == want)
                        at = c;
                }
            while (at >= 0 && at + 1 < sc->ncmd &&
                   sc->cmds[at + 1].kind == LS_OSEC &&
                   sc->osecs[sc->cmds[at + 1].osec].orphan)
                at++;
            struct ls_osec *o = lp_new_osec(sc, is->name);
            o->orphan = 1;
            o->file = "<orphan>";
            k = sc->nosec - 1;
            LS_PUSH(o->body, o->nbody, o->capbody);
            memset(&o->body[0], 0, sizeof o->body[0]);
            o->body[0].kind = LS_INPUT;
            o->nbody = 1;
            /* insert the command */
            LS_PUSH(sc->cmds, sc->ncmd, sc->capcmd);
            int pos = at < 0 ? sc->ncmd : at + 1;
            memmove(&sc->cmds[pos + 1], &sc->cmds[pos],
                    (size_t)(sc->ncmd - pos) * sizeof *sc->cmds);
            memset(&sc->cmds[pos], 0, sizeof sc->cmds[pos]);
            sc->cmds[pos].kind = LS_OSEC;
            sc->cmds[pos].osec = k;
            sc->cmds[pos].in_sections = 1;
            sc->ncmd++;
        }
        is->sosec = k;
        ls_push_list(&sc->osecs[k].body[0], si);
    }
}

static void ls_region_check(struct ls_state *st, const struct ls_osec *o,
                            int r, long long start, long long end)
{
    const struct ls_region *rg = &st->sc->reg[r];
    if (!st->final)
        return;
    if (start < rg->origin || start > rg->origin + rg->length)
        ls_die(st, "section %s at 0x%llx is not within region %s "
               "(0x%llx to 0x%llx)", o->name, (unsigned long long)start,
               rg->name, (unsigned long long)rg->origin,
               (unsigned long long)(rg->origin + rg->length));
    if (end > rg->origin + rg->length)
        ls_die(st, "region %s overflowed by %lld bytes (section %s ends "
               "at 0x%llx; the region ends at 0x%llx)", rg->name,
               end - (rg->origin + rg->length), o->name,
               (unsigned long long)end,
               (unsigned long long)(rg->origin + rg->length));
}

static void ls_layout_osec(struct ls_state *st, int k)
{
    struct linker *l = st->l;
    struct ls_script *sc = st->sc;
    struct ls_osec *o = &sc->osecs[k];
    long long start, lma, align = 1;
    int vr = -1, lr = -1;

    st->file = o->file;
    st->line = o->line;
    if (o->discard)
        return;
    o->has_input = 0;
    o->exec = o->write = 0;
    int has_bytes = 0, has_assign = 0;
    for (int b = 0; b < o->nbody; b++) {
        const struct ls_stmt *s = &o->body[b];
        if (s->kind == LS_ASSIGN)
            has_assign = 1;
        if (s->kind == LS_DATA)
            has_bytes = 1, o->has_input = 1;
        for (int j = 0; j < s->nlist; j++) {
            const struct insec *is = &l->insecs[s->list[j]];
            o->has_input = 1;
            if (is->align > (Elf64_Xword)align)
                align = (long long)is->align;
            if (is->shflags & SHF_EXECINSTR) o->exec = 1;
            if (is->shflags & SHF_WRITE) o->write = 1;
            if (!is->is_bss) has_bytes = 1;
        }
        if (s->kind == LS_INPUT && s->in.common)
            for (int i = 0; i < l->nsym; i++)
                if (l->syms[i].common) {
                    o->has_input = 1;
                    o->write = 1;
                    if ((long long)l->syms[i].align > align)
                        align = (long long)l->syms[i].align;
                }
    }
    /* ld drops an output section with nothing in it, even one with an
     * address (`.ARM.attributes 0 : { ... }` claims no allocated input);
     * one with assignments is kept where it stands, and occupies only
     * what its `.` moves cover */
    if (!o->has_input && !has_assign) {
        o->laid = 0;
        o->size = 0;
        o->vma = o->lma = st->dot;
        return;
    }
    if (o->align)
    {
        long long a2 = ls_eval_abs(st, o->align);
        if (a2 > align)
            align = a2;
    }
    if (o->vregion) {
        vr = ls_region_idx(sc, o->vregion);
        if (vr < 0)
            ls_die(st, "section %s: there is no memory region %s", o->name,
                   o->vregion);
    }
    if (o->lregion) {
        lr = ls_region_idx(sc, o->lregion);
        if (lr < 0)
            ls_die(st, "section %s: there is no memory region %s", o->name,
                   o->lregion);
    }
    if (o->addr) {
        int rel;
        start = ls_eval(st, o->addr, &rel);
    } else {
        start = ls_align_up(st, vr >= 0 ? sc->reg[vr].cur : st->dot, align);
    }
    /* No region named: the one the address falls in, if any, so that
     * its free byte and its load-address shift follow along. */
    if (vr < 0)
        for (int r = 0; r < sc->nreg; r++)
            if (start >= sc->reg[r].origin &&
                start < sc->reg[r].origin + sc->reg[r].length) {
                vr = r;
                break;
            }
    if (o->at)
        lma = ls_eval_abs(st, o->at);
    else if (lr >= 0)
        lma = ls_align_up(st, sc->reg[lr].cur, align);
    else if (vr >= 0 && sc->reg[vr].has_delta && !o->addr)
        lma = start + sc->reg[vr].delta;
    else
        lma = start;
    o->vma = start;
    o->lma = lma;
    o->al = align;
    st->cur = k;
    st->dot = start;
    for (int b = 0; b < o->nbody; b++) {
        struct ls_stmt *s = &o->body[b];
        st->file = s->file;
        st->line = s->line;
        switch (s->kind) {
        case LS_ASSIGN:
            ls_assign(st, s);
            break;
        case LS_INPUT: {
            long long sub = o->subalign ? ls_eval_abs(st, o->subalign) : 0;
            for (int j = 0; j < s->nlist; j++) {
                struct insec *is = &l->insecs[s->list[j]];
                st->dot = ls_align_up(st, st->dot,
                                      sub ? sub : (long long)is->align);
                is->vaddr = (Elf64_Addr)st->dot;
                st->dot += (long long)is->size;
            }
            if (s->in.common)
                for (int i = 0; i < l->nsym; i++) {
                    struct symbol *g = &l->syms[i];
                    if (!g->common)
                        continue;
                    st->dot = ls_align_up(st, st->dot,
                                          g->align ? (long long)g->align : 1);
                    g->value = (Elf64_Addr)st->dot;
                    st->dot += (long long)g->size;
                }
            break;
        }
        case LS_DATA:
            s->doff = st->dot - start;
            s->dval = ls_eval_abs(st, s->e);
            st->dot += s->dsize;
            break;
        case LS_FILL:
            o->fillv = ls_eval_abs(st, s->e);
            break;
        case LS_ASSERT:
            if (st->final && !ls_eval_abs(st, s->e))
                ls_die(st, "%s", s->msg);
            break;
        default:
            break;
        }
    }
    if (o->fill)
        o->fillv = ls_eval_abs(st, o->fill);
    o->size = st->dot - start;
    /* No bytes of its own -- .bss, or a section made only of `.` moves
     * like STM32's ._user_heap_stack -- takes no file space and no flash */
    o->nobits = o->noload || !has_bytes;
    o->laid = 1;
    st->cur = -1;
    if (vr >= 0) {
        ls_region_check(st, o, vr, start, start + o->size);
        if (start + o->size > sc->reg[vr].cur || o->vregion)
            sc->reg[vr].cur = start + o->size;
        sc->reg[vr].delta = lma - start;
        sc->reg[vr].has_delta = 1;
    }
    if (lr >= 0 && !o->nobits) {
        ls_region_check(st, o, lr, lma, lma + o->size);
        sc->reg[lr].cur = lma + o->size;
    }
    st->dot = start + o->size;
}

static void ls_layout_pass(struct linker *l, struct ls_script *sc, int final)
{
    struct ls_state st;
    memset(&st, 0, sizeof st);
    st.l = l;
    st.sc = sc;
    st.cur = -1;
    st.final = final;
    for (int r = 0; r < sc->nreg; r++) {
        struct ls_region *rg = &sc->reg[r];
        rg->origin = ls_eval_abs(&st, rg->org);
        rg->length = ls_eval_abs(&st, rg->len);
        rg->cur = rg->origin;
        rg->has_delta = 0;
        rg->delta = 0;
    }
    for (int c = 0; c < sc->ncmd; c++) {
        struct ls_stmt *s = &sc->cmds[c];
        st.in_sections = s->in_sections;
        st.file = s->file;
        st.line = s->line;
        if (s->kind == LS_OSEC)
            ls_layout_osec(&st, s->osec);
        else if (s->kind == LS_ASSIGN)
            ls_assign(&st, s);
        else if (s->kind == LS_ASSERT && final && !ls_eval_abs(&st, s->e))
            ls_die(&st, "%s", s->msg);
    }
}

/* Everything the layout decides, to compare one pass with the next. */
static unsigned long long ls_signature(struct linker *l, struct ls_script *sc)
{
    unsigned long long h = 1469598103934665603ULL;
#define MIX(x) (h = (h ^ (unsigned long long)(x)) * 1099511628211ULL)
    for (int k = 0; k < sc->nosec; k++) {
        MIX(sc->osecs[k].vma); MIX(sc->osecs[k].lma); MIX(sc->osecs[k].size);
    }
    for (int i = 0; i < l->nsym; i++)
        if (l->syms[i].scripted)
            MIX(l->syms[i].value);
    for (int i = 0; i < l->nsec; i++)
        MIX(l->insecs[i].vaddr);
#undef MIX
    return h;
}

static void ls_layout(struct linker *l, struct ls_script *sc, int orphan_mode)
{
    if (l->machine != EM_ARM && l->machine != EM_RISCV &&
        l->machine != EM_AVR)
        die("-T: a linker script is supported for ARM, RISC-V and AVR "
            "images only (this one is machine %u)", (unsigned)l->machine);
    /* A region every output section names must exist, whether or not
     * anything ends up in the section: a typo in `> RAM` is a typo. */
    for (int k = 0; k < sc->nosec; k++) {
        const struct ls_osec *o = &sc->osecs[k];
        const char *r[2] = { o->vregion, o->lregion };
        for (int j = 0; j < 2; j++)
            if (r[j] && ls_region_idx(sc, r[j]) < 0)
                die("%s:%d: section %s: there is no memory region %s",
                    o->file, o->line, o->name, r[j]);
    }
    ls_claim(l, sc);
    if (l->gc_sections) {
        /* after the claim, which is what knows the KEEP()s; and out of
         * every rule's list, so the layout never sees what went */
        gc_sections(l, l->gc_undefs, l->gc_nundefs);
        for (int k = 0; k < sc->nosec; k++)
            for (int b = 0; b < sc->osecs[k].nbody; b++) {
                struct ls_stmt *st = &sc->osecs[k].body[b];
                int n = 0;
                for (int j = 0; j < st->nlist; j++)
                    if (!l->insecs[st->list[j]].discarded)
                        st->list[n++] = st->list[j];
                st->nlist = n;
            }
    }
    {
        int ncommon = 0;
        for (int i = 0; i < l->nsym; i++)
            ncommon += l->syms[i].common;
        if (ncommon && !sc->has_common)
            die("%d COMMON symbol%s (tentative definitions from an object "
                "built with -fcommon) and the linker script places no "
                "*(COMMON): add it to the .bss output section, where the "
                "startup code zeroes it", ncommon, ncommon == 1 ? "" : "s");
    }
    ls_orphans(l, sc, orphan_mode);
    /* Settled; then the ARM veneers these addresses call for, which move
     * what follows them: settled again, until no branch wants a new one. */
    for (int round = 0;; round++) {
        unsigned long long prev = 0;
        int pass;
        for (pass = 0; pass < LS_MAX_PASSES; pass++) {
            ls_layout_pass(l, sc, 0);
            unsigned long long sig = ls_signature(l, sc);
            if (pass > 0 && sig == prev)
                break;
            prev = sig;
        }
        if (pass == LS_MAX_PASSES)
            die("the linker script's layout does not settle: an address "
                "depends on itself (a section placed by a symbol defined "
                "after it?)");
        if (!arm_veneer_scan(l))
            break;
        if (round == VENEER_MAX_ROUNDS)
            die("the long-branch veneers do not settle: each round's put "
                "another branch out of reach");
    }
    ls_layout_pass(l, sc, 1);        /* the same again, now reporting */
    /* AVR is a Harvard machine and EmbCC reads read-only data with
     * data-space loads (ld, lds), like avr-gcc without __flash or
     * PROGMEM. A .rodata left in program space -- run where it is stored,
     * below the data space at 0x800000 -- would link and then read
     * whatever RAM holds at those addresses. avr-libc's scripts put
     * .rodata in .data, which the startup copies to RAM; refuse the
     * layout that would silently do otherwise. */
    if (l->machine == EM_AVR)
        for (int k = 0; k < sc->nosec; k++) {
            const struct ls_osec *o = &sc->osecs[k];
            if (o->vma != o->lma || o->vma >= 0x800000)
                continue;
            for (int b2 = 0; b2 < o->nbody; b2++)
                for (int j = 0; j < o->body[b2].nlist; j++) {
                    const struct insec *in = &l->insecs[o->body[b2].list[j]];
                    if (strncmp(in->name, ".rodata", 7) == 0 && in->size)
                        die("%s:%d: section %s keeps %s of %s in program "
                            "space at 0x%llx; EmbCC reads read-only data "
                            "from RAM on AVR, so it has to be in an output "
                            "section the startup copies there (> data "
                            "AT> text, as avr-libc's scripts place it)",
                            o->file, o->line, o->name, in->name,
                            in->obj->name, (unsigned long long)o->vma);
                }
        }
    for (int i = 0; i < l->nsym; i++)
        if (l->syms[i].common) {     /* placed by *(COMMON): absolute now */
            l->syms[i].common = 0;
            l->syms[i].insec = -1;
        }
}

/* ---- writing a scripted image -------------------------------------------
 *
 * One PT_LOAD per output section that has bytes or size, with p_paddr
 * the section's LOAD address: that is what a flash programmer, objcopy
 * -O binary and QEMU's -kernel loader read to know where the bytes go,
 * and how .data's initial values end up in flash while its symbols say
 * RAM. One section header each, under the script's names, so size(1),
 * objdump and a debugger see the layout the script describes. */

static void ls_put_ehdr(unsigned char *img, const struct linker *l,
                        Elf64_Ehdr *eh)
{
    if (!l->elf32) {
        memcpy(img, eh, sizeof *eh);
        return;
    }
    Elf32_Ehdr e;
    memset(&e, 0, sizeof e);
    memcpy(e.e_ident, eh->e_ident, EI_NIDENT);
    e.e_type = eh->e_type;
    e.e_machine = eh->e_machine;
    e.e_version = eh->e_version;
    e.e_entry = (Elf32_Addr)eh->e_entry;
    e.e_phoff = (Elf32_Off)eh->e_phoff;
    e.e_shoff = (Elf32_Off)eh->e_shoff;
    e.e_flags = eh->e_flags;
    e.e_ehsize = eh->e_ehsize;
    e.e_phentsize = eh->e_phentsize;
    e.e_phnum = eh->e_phnum;
    e.e_shentsize = eh->e_shentsize;
    e.e_shnum = eh->e_shnum;
    e.e_shstrndx = eh->e_shstrndx;
    memcpy(img, &e, sizeof e);
}

static void ls_put_phdr(unsigned char *at, const struct linker *l,
                        const Elf64_Phdr *ph)
{
    if (!l->elf32) {
        memcpy(at, ph, sizeof *ph);
        return;
    }
    Elf32_Phdr p;
    p.p_type = ph->p_type;
    p.p_offset = (Elf32_Off)ph->p_offset;
    p.p_vaddr = (Elf32_Addr)ph->p_vaddr;
    p.p_paddr = (Elf32_Addr)ph->p_paddr;
    p.p_filesz = (Elf32_Word)ph->p_filesz;
    p.p_memsz = (Elf32_Word)ph->p_memsz;
    p.p_flags = ph->p_flags;
    p.p_align = (Elf32_Word)ph->p_align;
    memcpy(at, &p, sizeof p);
}

static void ls_put_shdr(unsigned char *at, const struct linker *l,
                        const Elf64_Shdr *sh)
{
    if (!l->elf32) {
        memcpy(at, sh, sizeof *sh);
        return;
    }
    Elf32_Shdr s;
    memset(&s, 0, sizeof s);
    s.sh_name = sh->sh_name;
    s.sh_type = sh->sh_type;
    s.sh_flags = (Elf32_Word)sh->sh_flags;
    s.sh_addr = (Elf32_Addr)sh->sh_addr;
    s.sh_offset = (Elf32_Off)sh->sh_offset;
    s.sh_size = (Elf32_Word)sh->sh_size;
    s.sh_link = sh->sh_link;
    s.sh_info = sh->sh_info;
    s.sh_addralign = (Elf32_Word)sh->sh_addralign;
    s.sh_entsize = (Elf32_Word)sh->sh_entsize;
    memcpy(at, &s, sizeof s);
}

static void ls_put_sym(unsigned char *at, const struct linker *l,
                       Elf64_Word name, Elf64_Addr val, Elf64_Xword size,
                       unsigned char info, Elf64_Half shndx)
{
    if (l->elf32) {
        Elf32_Sym e;
        memset(&e, 0, sizeof e);
        e.st_name = name;
        e.st_value = (Elf32_Addr)val;
        e.st_size = (Elf32_Word)size;
        e.st_info = info;
        e.st_shndx = shndx;
        memcpy(at, &e, sizeof e);
    } else {
        Elf64_Sym e;
        memset(&e, 0, sizeof e);
        e.st_name = name;
        e.st_value = val;
        e.st_size = size;
        e.st_info = info;
        e.st_shndx = shndx;
        memcpy(at, &e, sizeof e);
    }
}

static void write_exec_script(struct linker *l, struct ls_script *sc,
                              const char *out, Elf64_Addr entry)
{
    int *em = xmalloc((size_t)(sc->nosec + 1) * sizeof *em), nem = 0;
    for (int c = 0; c < sc->ncmd; c++) {
        if (sc->cmds[c].kind != LS_OSEC)
            continue;
        struct ls_osec *o = &sc->osecs[sc->cmds[c].osec];
        o->shndx = 0;
        if (!o->discard && o->laid && o->size > 0)
            em[nem++] = sc->cmds[c].osec;
    }
    Elf64_Off ehsz = l->elf32 ? sizeof(Elf32_Ehdr) : sizeof(Elf64_Ehdr);
    Elf64_Off phsz = l->elf32 ? sizeof(Elf32_Phdr) : sizeof(Elf64_Phdr);
    Elf64_Xword shentsz = l->elf32 ? sizeof(Elf32_Shdr) : sizeof(Elf64_Shdr);
    Elf64_Xword symentsz = l->elf32 ? sizeof(Elf32_Sym) : sizeof(Elf64_Sym);
    Elf64_Off off = ehsz + (Elf64_Off)nem * phsz;
    Elf64_Off *fo = xmalloc((size_t)(nem + 1) * sizeof *fo);
    for (int k = 0; k < nem; k++) {
        struct ls_osec *o = &sc->osecs[em[k]];
        /* the file offset congruent to the address modulo 4, which is
         * all a PT_LOAD aligned to 4 asks */
        off = align_up(off, 4) + ((Elf64_Off)o->vma & 3);
        fo[k] = off;
        o->shndx = 1 + k;
        if (!o->nobits)
            off += (Elf64_Off)o->size;
    }
    /* section headers: [0] null, the output sections, the DWARF, then
     * .symtab .strtab .shstrtab */
    int sh_dbg0 = 1 + nem, sh_symtab = sh_dbg0 + l->ndbg;
    int sh_strtab = sh_symtab + 1, sh_shstr = sh_symtab + 2;
    int nsh = sh_shstr + 1;

    struct ltab symstr, shstr;
    memset(&symstr, 0, sizeof symstr);
    memset(&shstr, 0, sizeof shstr);
    ltab_add(&symstr, "");
    ltab_add(&shstr, "");
    Elf64_Word *n_osec = xmalloc((size_t)(nem + 1) * sizeof *n_osec);
    for (int k = 0; k < nem; k++)
        n_osec[k] = (Elf64_Word)ltab_add(&shstr, sc->osecs[em[k]].name);
    Elf64_Word *n_dbg = xmalloc((size_t)(l->ndbg + 1) * sizeof *n_dbg);
    for (int i = 0; i < l->ndbg; i++)
        n_dbg[i] = (Elf64_Word)ltab_add(&shstr, l->dbgsecs[i].name);
    Elf64_Word n_symtab = (Elf64_Word)ltab_add(&shstr, ".symtab");
    Elf64_Word n_strtab = (Elf64_Word)ltab_add(&shstr, ".strtab");
    Elf64_Word n_shstr = (Elf64_Word)ltab_add(&shstr, ".shstrtab");

    int nsy = 0;
    Elf64_Word *symname = xmalloc((size_t)(l->nsym + 1) * sizeof *symname);
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *g = &l->syms[i];
        if (sym_in_image(l, g))
            symname[nsy++] = (Elf64_Word)ltab_add(&symstr, g->name);
    }

    Elf64_Off dbg_off0 = align_up(off, 8), dbg_total = 0;
    Elf64_Off *dbg_at = xmalloc((size_t)(l->ndbg + 1) * sizeof *dbg_at);
    for (int i = 0; i < l->ndbg; i++) {
        dbg_at[i] = dbg_off0 + dbg_total;
        dbg_total += (Elf64_Off)l->dbgsecs[i].len;
    }
    Elf64_Off sym_off = align_up(dbg_off0 + dbg_total, 8);
    Elf64_Off symsz = (Elf64_Off)(nsy + 1) * symentsz;
    Elf64_Off str_off = sym_off + symsz;
    Elf64_Off shstr_off = str_off + symstr.len;
    Elf64_Off sh_off = align_up(shstr_off + shstr.len, 8);
    Elf64_Off total = sh_off + (Elf64_Off)nsh * shentsz;
    unsigned char *img = xcalloc(1, (size_t)total);

    /* the bytes: fill, then each input where the layout put it, then the
     * BYTE/SHORT/LONG/QUAD statements */
    for (int k = 0; k < nem; k++) {
        struct ls_osec *o = &sc->osecs[em[k]];
        if (o->nobits)
            continue;
        unsigned char pat[4];
        int plen;
        unsigned long fv = (unsigned long)o->fillv;
        if (fv <= 0xff) {
            pat[0] = (unsigned char)fv;
            plen = 1;
        } else {                        /* big-endian, as ld writes it */
            pat[0] = (unsigned char)(fv >> 24); pat[1] = (unsigned char)(fv >> 16);
            pat[2] = (unsigned char)(fv >> 8);  pat[3] = (unsigned char)fv;
            plen = 4;
        }
        if (fv)
            for (long long b = 0; b < o->size; b++)
                img[fo[k] + (Elf64_Off)b] = pat[b % plen];
        for (int bb = 0; bb < o->nbody; bb++) {
            struct ls_stmt *s = &o->body[bb];
            for (int j = 0; j < s->nlist; j++) {
                struct insec *is = &l->insecs[s->list[j]];
                if (is->is_bss || !is->data || !is->size)
                    continue;
                memcpy(img + fo[k] + (is->vaddr - (Elf64_Addr)o->vma),
                       is->data, (size_t)is->size);
                /* NOBITS inputs inside a PROGBITS output section are
                 * its zeros, already there */
            }
            if (s->kind == LS_DATA) {
                unsigned long long v = (unsigned long long)s->dval;
                for (int b = 0; b < s->dsize; b++)    /* the target's order */
                    img[fo[k] + (Elf64_Off)s->doff +
                        (Elf64_Off)(l->big_endian ? s->dsize - 1 - b : b)] =
                        (unsigned char)(v >> (8 * b));
            }
        }
        /* a NOBITS input claimed into a section with bytes is zeros */
        for (int bb = 0; bb < o->nbody; bb++)
            for (int j = 0; j < o->body[bb].nlist; j++) {
                struct insec *is = &l->insecs[o->body[bb].list[j]];
                if (is->is_bss && is->size)
                    memset(img + fo[k] + (is->vaddr - (Elf64_Addr)o->vma), 0,
                           (size_t)is->size);
            }
    }

    Elf64_Ehdr eh;
    memset(&eh, 0, sizeof eh);
    eh.e_ident[EI_MAG0] = ELFMAG0;
    eh.e_ident[EI_MAG1] = ELFMAG1;
    eh.e_ident[EI_MAG2] = ELFMAG2;
    eh.e_ident[EI_MAG3] = ELFMAG3;
    eh.e_ident[EI_CLASS] = l->elf32 ? ELFCLASS32 : ELFCLASS64;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_EXEC;
    eh.e_machine = (Elf64_Half)l->machine;
    eh.e_version = EV_CURRENT;
    eh.e_entry = entry;
    eh.e_flags = l->machine == EM_ARM ? EF_ARM_EABI_VER5 : l->eflags;
    eh.e_phoff = nem ? ehsz : 0;
    eh.e_shoff = sh_off;
    eh.e_ehsize = (Elf64_Half)ehsz;
    eh.e_phentsize = (Elf64_Half)phsz;
    eh.e_phnum = (Elf64_Half)nem;
    eh.e_shentsize = (Elf64_Half)shentsz;
    eh.e_shnum = (Elf64_Half)nsh;
    eh.e_shstrndx = (Elf64_Half)sh_shstr;
    ls_put_ehdr(img, l, &eh);

    for (int k = 0; k < nem; k++) {
        struct ls_osec *o = &sc->osecs[em[k]];
        Elf64_Phdr ph;
        memset(&ph, 0, sizeof ph);
        ph.p_type = PT_LOAD;
        ph.p_flags = PF_R | (o->exec ? PF_X : 0) | (o->write ? PF_W : 0);
        ph.p_offset = fo[k];
        ph.p_vaddr = (Elf64_Addr)o->vma;
        /* A NOBITS section has nothing to load. A loader still zeroes
         * its p_memsz at p_paddr -- QEMU's does -- so that is its run
         * address, where zeroing is harmless, and not a load address in
         * flash that it could overlap. */
        ph.p_paddr = (Elf64_Addr)(o->nobits ? o->vma : o->lma);
        ph.p_filesz = o->nobits ? 0 : (Elf64_Xword)o->size;
        ph.p_memsz = (Elf64_Xword)o->size;
        ph.p_align = 4;
        ls_put_phdr(img + ehsz + (Elf64_Off)k * phsz, l, &ph);
    }

    Elf64_Shdr sh;
    for (int k = 0; k < nem; k++) {
        struct ls_osec *o = &sc->osecs[em[k]];
        memset(&sh, 0, sizeof sh);
        sh.sh_name = n_osec[k];
        sh.sh_type = o->nobits ? SHT_NOBITS : SHT_PROGBITS;
        sh.sh_flags = SHF_ALLOC | (o->exec ? SHF_EXECINSTR : 0) |
                      (o->write ? SHF_WRITE : 0);
        /* The inputs' own type when they all have one, as ld keeps it:
         * .init_array stays INIT_ARRAY, and .ARM.exidx stays ARM_EXIDX
         * and links to where its functions went -- which is what
         * llvm-readelf -u and a debugger look for. */
        {
            Elf64_Word ty = 0;
            int same = 1, first = -1;
            for (int b = 0; b < o->nbody && same; b++)
                for (int j = 0; j < o->body[b].nlist && same; j++) {
                    const struct insec *is = &l->insecs[o->body[b].list[j]];
                    Elf64_Word t = is->obj
                        ? sh_at(is->obj, is->shndx)->sh_type : SHT_PROGBITS;
                    if (first < 0) { ty = t; first = o->body[b].list[j]; }
                    else if (t != ty) same = 0;
                }
            if (same && first >= 0 && ty != SHT_PROGBITS && ty != SHT_NOBITS) {
                sh.sh_type = ty;
                const struct insec *is = &l->insecs[first];
                Elf64_Shdr *ish = sh_at(is->obj, is->shndx);
                if (ish->sh_flags & SHF_LINK_ORDER) {
                    sh.sh_flags |= SHF_LINK_ORDER;
                    int ln = ish->sh_link < (Elf64_Word)is->obj->nsh
                           ? is->obj->sec_out[ish->sh_link] : -1;
                    int so = ln >= 0 ? l->insecs[ln].sosec : -1;
                    for (int j = 0; j < nem && so >= 0; j++)
                        if (em[j] == so)
                            sh.sh_link = (Elf64_Word)(1 + j);
                }
            }
        }
        sh.sh_addr = (Elf64_Addr)o->vma;
        sh.sh_offset = fo[k];
        sh.sh_size = (Elf64_Xword)o->size;
        sh.sh_addralign = (Elf64_Xword)(o->al > 0 ? o->al : 1);
        ls_put_shdr(img + sh_off + (Elf64_Off)(1 + k) * shentsz, l, &sh);
    }
    for (int i = 0; i < l->ndbg; i++) {
        memset(&sh, 0, sizeof sh);
        sh.sh_name = n_dbg[i];
        sh.sh_type = SHT_PROGBITS;
        sh.sh_offset = dbg_at[i];
        sh.sh_size = (Elf64_Xword)l->dbgsecs[i].len;
        sh.sh_addralign = 1;
        memcpy(img + dbg_at[i], l->dbgsecs[i].data, (size_t)l->dbgsecs[i].len);
        ls_put_shdr(img + sh_off + (Elf64_Off)(sh_dbg0 + i) * shentsz, l, &sh);
    }
    memset(&sh, 0, sizeof sh);
    sh.sh_name = n_symtab;
    sh.sh_type = SHT_SYMTAB;
    sh.sh_offset = sym_off;
    sh.sh_size = symsz;
    sh.sh_link = (Elf64_Word)sh_strtab;
    sh.sh_info = 1;
    sh.sh_addralign = 8;
    sh.sh_entsize = symentsz;
    ls_put_shdr(img + sh_off + (Elf64_Off)sh_symtab * shentsz, l, &sh);
    memset(&sh, 0, sizeof sh);
    sh.sh_name = n_strtab;
    sh.sh_type = SHT_STRTAB;
    sh.sh_offset = str_off;
    sh.sh_size = (Elf64_Xword)symstr.len;
    sh.sh_addralign = 1;
    ls_put_shdr(img + sh_off + (Elf64_Off)sh_strtab * shentsz, l, &sh);
    memset(&sh, 0, sizeof sh);
    sh.sh_name = n_shstr;
    sh.sh_type = SHT_STRTAB;
    sh.sh_offset = shstr_off;
    sh.sh_size = (Elf64_Xword)shstr.len;
    sh.sh_addralign = 1;
    ls_put_shdr(img + sh_off + (Elf64_Off)sh_shstr * shentsz, l, &sh);
    memcpy(img + str_off, symstr.p, (size_t)symstr.len);
    memcpy(img + shstr_off, shstr.p, (size_t)shstr.len);

    int j = 0;
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *g = &l->syms[i];
        if (!sym_in_image(l, g))
            continue;
        Elf64_Half shndx = SHN_ABS;
        int exec = 0;
        if (g->scripted) {
            if (g->sosec >= 0 && sc->osecs[g->sosec].shndx)
                shndx = (Elf64_Half)sc->osecs[g->sosec].shndx;
        } else if (g->insec >= 0) {
            int so = l->insecs[g->insec].sosec;
            if (so >= 0 && sc->osecs[so].shndx) {
                shndx = (Elf64_Half)sc->osecs[so].shndx;
                exec = sc->osecs[so].exec;
            }
        }
        int type = g->scripted ? STT_NOTYPE
                 : g->type ? g->type : exec ? STT_FUNC : STT_OBJECT;
        ls_put_sym(img + sym_off + (Elf64_Off)(j + 1) * symentsz, l,
                   symname[j], g->value, g->size,
                   (unsigned char)ELF64_ST_INFO(STB_GLOBAL, type), shndx);
        j++;
    }
    if (l->big_endian) {
        if (l->elf32) be_image(img);
        else          be_image64(img);
    }
    if (plat_write_file(out, img, (size_t)total) != 0)
        die("cannot write '%s'", out);
    free(img); free(em); free(fo); free(n_osec); free(n_dbg); free(dbg_at);
    free(symname); free(symstr.p); free(shstr.p);
}

/* A file a script names (INPUT, GROUP, STARTUP): `-lNAME` is libNAME.a
 * in the search directories; anything else is found as given, or there. */
/* ---- -Map and --print-memory-usage -----------------------------------
 *
 * The map is GNU ld's, in its sections and their shape, so that what a
 * firmware developer reads -- and what tools written for ld's maps parse
 * -- is where it always is: which archive members were pulled and for
 * whom, what --gc-sections removed, the memory regions, then each output
 * section with its address and size, the input sections in it, and the
 * global symbols each defines. */

struct mapw { FILE *f; int w; };    /* w: hex digits of an address */

static void map_addr(struct mapw *m, unsigned long long v)
{
    fprintf(m->f, "0x%0*llx", m->w, v);
}

static void map_size(struct mapw *m, unsigned long long v)
{
    char b[32];
    snprintf(b, sizeof b, "0x%llx", v);
    fprintf(m->f, "%*s", 10, b);
}

/* " NAME" padded to ld's 16 columns, or on a line of its own if longer */
static void map_name(struct mapw *m, const char *lead, const char *name)
{
    int len = fprintf(m->f, "%s%s", lead, name);
    if (len >= 15) {
        fputc('\n', m->f);
        len = 0;
    }
    fprintf(m->f, "%*s", 16 - len, "");
}

static int map_sym_cmp(const void *a, const void *b)
{
    const struct symbol *x = *(const struct symbol *const *)a,
                        *y = *(const struct symbol *const *)b;
    return x->value < y->value ? -1 : x->value > y->value;
}

static void map_input(struct linker *l, struct mapw *m,
                      struct symbol **byaddr, int nby, int si)
{
    const struct insec *is = &l->insecs[si];
    map_name(m, " ", is->name);
    map_addr(m, is->vaddr);
    fputc(' ', m->f);
    map_size(m, is->size);
    fprintf(m->f, " %s\n", is->obj ? is->obj->name : "*linker stub*");
    for (int k = 0; k < nby; k++)
        if (byaddr[k]->insec == si) {
            fprintf(m->f, "%16s", "");
            map_addr(m, byaddr[k]->value);
            fprintf(m->f, "%16s%s\n", "", byaddr[k]->name);
        }
}

static const char *const osec_names[OSEC_COUNT] = {
    ".vectors", ".text", ".rodata", ".tdata", ".tbss", ".init_array",
    ".fini_array", ".ctors", ".dtors", ".data", ".bss",
};

static void write_map(struct linker *l, struct ls_script *sc,
                      const struct osec_bound *b, const char *path)
{
    struct mapw m;
    m.f = fopen(path, "w");
    if (!m.f)
        die("cannot write the map file '%s'", path);
    m.w = l->elf32 ? 8 : 16;

    int hdr = 0;
    for (int i = 0; i < l->nobj; i++) {
        struct object *o = l->objs[i];
        if (!o->pulled_for)
            continue;
        if (!hdr++)
            fprintf(m.f, "Archive member included to satisfy reference by "
                    "file (symbol)\n\n");
        int len = fprintf(m.f, "%s", o->name);
        if (len >= 29) {
            fputc('\n', m.f);
            len = 0;
        }
        fprintf(m.f, "%*s%s (%s)\n", 30 - len, "",
                o->pulled_by ? o->pulled_by->name : "(command line)",
                o->pulled_for);
    }
    if (hdr)
        fputc('\n', m.f);

    hdr = 0;
    for (int i = 0; i < l->nsec; i++) {
        const struct insec *is = &l->insecs[i];
        if (!is->discarded)
            continue;
        if (!hdr++)
            fprintf(m.f, "Discarded input sections\n\n");
        map_name(&m, " ", is->name);
        map_addr(&m, 0);
        fputc(' ', m.f);
        map_size(&m, is->size);
        fprintf(m.f, " %s\n", is->obj ? is->obj->name : "?");
    }
    if (hdr)
        fputc('\n', m.f);

    fprintf(m.f, "Memory Configuration\n\nName             Origin             "
            "Length             Attributes\n");
    for (int r = 0; sc && r < sc->nreg; r++) {
        const struct ls_region *rg = &sc->reg[r];
        fprintf(m.f, "%-16s ", rg->name);
        map_addr(&m, (unsigned long long)rg->origin);
        fprintf(m.f, "%*s", 18 - m.w - 2 + 1, "");
        map_addr(&m, (unsigned long long)rg->length);
        fprintf(m.f, "%*s%s\n", 18 - m.w - 2 + 1, "", rg->attrs ? rg->attrs : "");
    }
    fprintf(m.f, "%-16s ", "*default*");
    map_addr(&m, 0);
    fprintf(m.f, "%*s", 18 - m.w - 2 + 1, "");
    map_addr(&m, l->elf32 ? 0xffffffffULL : ~0ULL);
    fprintf(m.f, "\n\nLinker script and memory map\n\n");

    /* global symbols by address, for the lines under each input */
    struct symbol **byaddr = xmalloc((size_t)(l->nsym ? l->nsym : 1) *
                                     sizeof *byaddr);
    int nby = 0;
    for (int i = 0; i < l->nsym; i++)
        if (sym_in_image(l, &l->syms[i]) && l->syms[i].insec >= 0 &&
            !l->syms[i].scripted)
            byaddr[nby++] = &l->syms[i];
    qsort(byaddr, (size_t)nby, sizeof *byaddr, map_sym_cmp);

    if (sc) {
        for (int c = 0; c < sc->ncmd; c++) {
            if (sc->cmds[c].kind != LS_OSEC)
                continue;
            const struct ls_osec *o = &sc->osecs[sc->cmds[c].osec];
            if (o->discard || !o->laid)
                continue;
            map_name(&m, "", o->name);
            map_addr(&m, (unsigned long long)o->vma);
            fputc(' ', m.f);
            map_size(&m, (unsigned long long)o->size);
            if (o->lma != o->vma) {
                fprintf(m.f, " load address ");
                map_addr(&m, (unsigned long long)o->lma);
            }
            fputc('\n', m.f);
            for (int bb = 0; bb < o->nbody; bb++)
                for (int j = 0; j < o->body[bb].nlist; j++)
                    map_input(l, &m, byaddr, nby, o->body[bb].list[j]);
            fputc('\n', m.f);
        }
    } else {
        for (int os = 0; os < OSEC_COUNT + l->norphan; os++) {
            int any = 0;
            for (int i = 0; i < l->nsec && !any; i++)
                any = l->insecs[i].osec == os && !l->insecs[i].discarded;
            if (!any)
                continue;
            map_name(&m, "", os < OSEC_COUNT ? osec_names[os]
                                             : l->orphans[os - OSEC_COUNT].name);
            map_addr(&m, b[os].start);
            fputc(' ', m.f);
            map_size(&m, b[os].end - b[os].start);
            fputc('\n', m.f);
            for (int i = 0; i < l->nsec; i++)
                if (l->insecs[i].osec == os && !l->insecs[i].discarded)
                    map_input(l, &m, byaddr, nby, i);
            fputc('\n', m.f);
        }
    }
    free(byaddr);
    if (fclose(m.f) != 0)
        die("cannot write the map file '%s'", path);
}

/* ld's own table and number format, so a build log reads the same */
static void mem_size(unsigned long long v)
{
    if (v && (v & 0x3fffffffULL) == 0)
        printf("%10llu GB", v >> 30);
    else if (v && (v & 0xfffffULL) == 0)
        printf("%10llu MB", v >> 20);
    else if (v && (v & 0x3ffULL) == 0)
        printf("%10llu KB", v >> 10);
    else
        printf("%10llu B", v);
}

static void print_memory_usage(const struct ls_script *sc)
{
    printf("Memory region         Used Size  Region Size  %%age Used\n");
    for (int r = 0; sc && r < sc->nreg; r++) {
        const struct ls_region *rg = &sc->reg[r];
        unsigned long long used = (unsigned long long)(rg->cur - rg->origin);
        printf("%16s: ", rg->name);
        mem_size(used);
        mem_size((unsigned long long)rg->length);
        printf("    %6.2f%%\n",
               rg->length ? 100.0 * (double)used / (double)rg->length : 0.0);
    }
}

static const char *ls_find_input(const char *name, const char **dirs,
                                 int ndirs)
{
    char buf[4096];
    FILE *f;
    if (!strncmp(name, "-l", 2)) {
        for (int k = 0; k < ndirs; k++) {
            snprintf(buf, sizeof buf, "%s/lib%s.a", dirs[k], name + 2);
            if ((f = fopen(buf, "rb")) != NULL) {
                fclose(f);
                return xstrndup(buf, strlen(buf));
            }
        }
        die("cannot find lib%s.a (from %s in the linker script) in any "
            "search directory", name + 2, name);
    }
    if ((f = fopen(name, "rb")) != NULL) {
        fclose(f);
        return name;
    }
    for (int k = 0; k < ndirs; k++) {
        snprintf(buf, sizeof buf, "%s/%s", dirs[k], name);
        if ((f = fopen(buf, "rb")) != NULL) {
            fclose(f);
            return xstrndup(buf, strlen(buf));
        }
    }
    die("cannot find '%s', which the linker script names as an input", name);
    return NULL;
}

/* ---- ARMv8-M's security extension: CMSE -----------------------------------
 *
 * A cmse_nonsecure_entry function arrives as two global symbols at one
 * address, `f` and `__acle_se_f` (ACLE; src/driver writes both). For each
 * such pair the linker makes a SECURE GATEWAY VENEER in .gnu.sgstubs --
 *
 *      f:  sg                      e97f e97f
 *          b.w  __acle_se_f
 *
 * -- and `f` comes to name the veneer, as GNU ld does: a Non-secure caller
 * can only enter the Secure state at an SG in memory the SAU/IDAU marks
 * Non-secure Callable, which is where the image's .gnu.sgstubs is put, and
 * `__acle_se_f` still names the code for a Secure caller that wants to skip
 * the gateway. Eight bytes each, in name order, the section 32-byte aligned
 * and padded to 32 bytes, the SAU's granule.
 *
 * --cmse-implib --out-implib=FILE then writes the IMPORT LIBRARY: an ELF
 * relocatable holding nothing but an absolute global function symbol per
 * veneer, which the Non-secure image links against.
 *
 * And that image's calls into it reach a Secure address the Non-secure code
 * is usually more than a BL's 16 MiB from (on the mps2-an505, 0x00200000
 * against 0x10000000): they go through long-branch veneers, as every
 * call out of reach does (arm_veneers below). */
static struct linker *g_sg_l;
static int sg_cmp(const void *pa, const void *pb)
{
    const int *a = pa, *b = pb;
    return strcmp(g_sg_l->syms[a[0]].name, g_sg_l->syms[b[0]].name);
}

/* A section of the linker's own, with no object behind it. */
static int add_synth(struct linker *l, const char *name, long size, int align,
                     int osec, unsigned char *data)
{
    struct insec *s;
    if (l->nsec == l->capsec) {
        l->capsec = l->capsec ? l->capsec * 2 : 64;
        l->insecs = xrealloc(l->insecs, (size_t)l->capsec * sizeof *l->insecs);
    }
    s = &l->insecs[l->nsec];
    memset(s, 0, sizeof *s);
    s->obj = NULL;
    s->shndx = -1;
    s->name = name;
    s->size = (Elf64_Xword)size;
    s->align = (Elf64_Xword)align;
    s->data = data;
    s->osec = osec;
    s->seg = SEG_TEXT;
    s->sosec = -1;
    s->shflags = SHF_ALLOC | SHF_EXECINSTR;
    return l->nsec++;
}

static void cmse_scan(struct linker *l)
{
    int (*pairs)[2] = NULL;
    int n = 0;
    l->sg_sec = -1;
    if (l->machine != EM_ARM)
        return;
    for (int i = 0; i < l->nsym; i++) {
        struct symbol *se = &l->syms[i], *g;
        if (!se->defined || !se->name || strncmp(se->name, "__acle_se_", 10))
            continue;
        g = sym_find(l, se->name + 10);
        if (!g || !g->defined || g->insec < 0 || g->insec != se->insec ||
            g->value != se->value)
            die("'%s' has no global '%s' at the same address: a "
                "cmse_nonsecure_entry function is the pair (ACLE), and the "
                "secure gateway veneer is built from it", se->name,
                se->name + 10);
        if (!(se->value & 1))
            die("'%s' is not Thumb code, and a secure gateway veneer can "
                "only branch to Thumb code", se->name);
        pairs = xrealloc(pairs, (size_t)(n + 1) * sizeof *pairs);
        pairs[n][0] = (int)(g - l->syms);
        pairs[n][1] = i;
        n++;
    }
    if (!n)
        return;
    g_sg_l = l;
    qsort(pairs, (size_t)n, sizeof *pairs, sg_cmp);
    {
        long size = ((long)n * 8 + 31) & ~31L;
        l->sg_sec = add_synth(l, ".gnu.sgstubs", size, 32,
                              orphan_osec(l, ".gnu.sgstubs", 0),
                              xcalloc((size_t)size, 1));
    }
    l->sg = xmalloc((size_t)n * sizeof *l->sg);
    for (int k = 0; k < n; k++) {
        struct symbol *g = &l->syms[pairs[k][0]];
        l->sg[k].sym = pairs[k][0];
        l->sg[k].se = pairs[k][1];
        g->insec = l->sg_sec;
        g->value = (Elf64_Addr)(8 * k) | 1;
        g->size = 8;
        g->type = STT_FUNC;
    }
    l->nsg = n;
    free(pairs);
}

/* ---- ARM long-branch veneers (arm_veneers) ------------------------------
 *
 * A Thumb `bl` or `b.w` reaches 16 MB either way and an ARM `bl` or `b`
 * 32 MB, and a function run from RAM -- an STM32's SRAM is at 0x20000000,
 * its flash at 0x08000000 -- is 384 MB from the code it calls. GNU ld
 * answers with a VENEER: a stub the branch can reach, which goes the rest
 * of the way through a register. This linker mints the same, AFTER a
 * layout, since which branches reach is a question of addresses, and AT
 * THE END OF THE CALLER'S OUTPUT SECTION, since a veneer in flash is no
 * nearer to the RAM code than its callee was. The veneers move whatever
 * follows them, so the layout is run again, until no branch wants a new
 * one; a veneer once minted is kept, so that ends.
 *
 * Twelve bytes each, four-aligned. From Thumb code: `movw ip, #lo; movt
 * ip, #hi; bx ip`, or on ARMv6-M, which has no MOVW, `push {r0, r1}; ldr
 * r0, [pc, #4]; str r0, [sp, #4]; pop {r0, pc}` and the address. From ARM
 * code: `ldr ip, [pc]; bx ip` and the address. IP is the register the
 * AAPCS gives a veneer. `bx`, and the pop into pc, take the target's
 * instruction state from the address's low bit, so a branch that must
 * change state and cannot -- a jump either way, or a conditional ARM
 * call, which no blx encodes -- goes through one whatever its distance.
 *
 * One veneer serves every branch from its output section to one target:
 * a symbol plus an addend, since an assembler spells a call to a static
 * function as its section plus an offset. An absolute target -- a CMSE
 * import library's gateway -- is the same case as any other. */
static void arm_veneer_init(struct linker *l)
{
    if (l->machine != EM_ARM)
        return;
    for (int i = 0; i < l->nobj; i++) {
        struct object *o = l->objs[i];
        /* Tag_CPU_arch 11 or 12 (v6-M, v6S-M), stored plus one */
        if (o->elf32 && (o->arm_arch == 12 || o->arm_arch == 13))
            l->lb_thumb1 = 1;
    }
}

/* Whether a branch whose displacement would be `d` needs a veneer: it is
 * out of the branch's reach, or it must change state and cannot. The
 * same question is asked when the veneers are decided and when the
 * branch is patched, so the two agree. */
static int veneer_needed(int arm, int call, int cond_al, int other,
                         long long d)
{
    int bits = arm ? 25 : 24;
    if (d < -(1LL << bits) || d >= (1LL << bits))
        return 1;
    return other && (!call || !cond_al);
}

/* Which output section a branch is in: the key its veneer is kept under. */
static int veneer_key(const struct linker *l, const struct insec *s)
{
    return l->sc ? s->sosec : s->osec;
}

/* The global a relocation's symbol names, or -1 for a local one. */
static int veneer_gsym(struct linker *l, struct object *o, Elf64_Word symi)
{
    Elf64_Sym *sy = &o->syms[symi];
    const char *name = o->symstr + sy->st_name;
    struct symbol *g;
    if (ELF64_ST_BIND(sy->st_info) == STB_LOCAL || !*name)
        return -1;
    g = sym_find(l, name);
    return g ? (int)(g - l->syms) : -1;
}

static int veneer_find(struct linker *l, int key, int gsym, int obj,
                       Elf64_Word symi, long long A, int arm)
{
    for (int k = 0; k < l->nvn; k++) {
        const struct veneer *v = &l->vn[k];
        if (v->key != key || v->arm != arm || v->addend != A)
            continue;
        if (gsym >= 0 ? v->gsym == gsym
                      : v->gsym < 0 && v->obj == obj && v->symi == (int)symi)
            return k;
    }
    return -1;
}

/* A branch's target BEFORE finalize_symbols, which is when the veneers
 * are decided: its section's address plus its offset in it. *ok is
 * cleared for a target with no address (undefined, or in a section the
 * link dropped), which gets no veneer. */
static Elf64_Addr veneer_target(struct linker *l, struct object *o,
                                Elf64_Word symi, int gsym, int *ok)
{
    Elf64_Sym *sy = &o->syms[symi];
    int out;
    *ok = 1;
    if (gsym >= 0) {
        struct symbol *g = &l->syms[gsym];
        if (!g->defined) {
            *ok = 0;
            return 0;
        }
        if (g->insec < 0)
            return g->value;
        if (l->insecs[g->insec].discarded) {
            *ok = 0;
            return 0;
        }
        return l->insecs[g->insec].vaddr + g->value;
    }
    if (ELF64_ST_BIND(sy->st_info) != STB_LOCAL) {
        *ok = 0;
        return 0;
    }
    if (sy->st_shndx == SHN_ABS)
        return sy->st_value;
    out = sy->st_shndx < (unsigned)o->nsh ? o->sec_out[sy->st_shndx] : -1;
    if (out < 0 || l->insecs[out].discarded) {
        *ok = 0;
        return 0;
    }
    return l->insecs[out].vaddr + sy->st_value;
}

/* Each new veneer's twelve bytes, in its output section's veneer section
 * -- made at the first one, and appended to that output section: the
 * fixed group's last member, or (-T) the last input of the script's
 * section. The bytes are written after the final layout (arm_veneer_fill). */
static void arm_veneer_place(struct linker *l)
{
    for (int k = 0; k < l->nvn; k++) {
        struct veneer *v = &l->vn[k];
        struct veneer_sec *vs = NULL;
        struct insec *s;
        if (v->sec >= 0)
            continue;
        for (int j = 0; j < l->nvns; j++)
            if (l->vns[j].key == v->key)
                vs = &l->vns[j];
        if (!vs) {
            int sec = add_synth(l, ".text.__embld_veneer", 0, 4,
                                l->sc ? OSEC_TEXT : v->key, NULL);
            if (l->sc) {
                struct ls_osec *o = &l->sc->osecs[v->key];
                int b;
                for (b = o->nbody - 1; b >= 0 && o->body[b].kind != LS_INPUT;
                     b--)
                    ;
                if (b < 0) {
                    LS_PUSH(o->body, o->nbody, o->capbody);
                    memset(&o->body[o->nbody], 0, sizeof o->body[0]);
                    o->body[o->nbody].kind = LS_INPUT;
                    b = o->nbody++;
                }
                l->insecs[sec].sosec = v->key;
                ls_push_list(&o->body[b], sec);
            } else {
                /* the callers' segment: a veneer in .data is copied to
                 * RAM with the function it serves */
                int key = v->key;
                l->insecs[sec].seg =
                    key == OSEC_VECTORS || key == OSEC_TEXT ||
                    key == OSEC_RODATA ||
                    (key >= OSEC_COUNT && !l->orphans[key - OSEC_COUNT].writable)
                        ? SEG_TEXT : SEG_DATA;
            }
            l->insecs[sec].keep = 1;
            l->insecs[sec].live = 1;
            LS_PUSH(l->vns, l->nvns, l->capvns);
            vs = &l->vns[l->nvns++];
            vs->key = v->key;
            vs->sec = sec;
            vs->n = 0;
        }
        s = &l->insecs[vs->sec];
        v->sec = vs->sec;
        v->k = vs->n++;
        s->size = 12 * (Elf64_Xword)vs->n;
        s->data = xrealloc((void *)s->data, (size_t)s->size);
        memset((unsigned char *)s->data + 12 * v->k, 0, 12);
    }
}

/* After a layout: a veneer for every branch that needs one and has none.
 * How many were added; the layout is run again when any was. */
static int arm_veneer_scan(struct linker *l)
{
    int added = 0;
    if (l->machine != EM_ARM)
        return 0;
    for (int oi = 0; oi < l->nobj; oi++) {
        struct object *o = l->objs[oi];
        if (!o->elf32 || o->machine != EM_ARM)
            continue;
        for (int si = 1; si < o->nsh; si++) {
            Elf64_Shdr *rsh = sh_at(o, si);
            int isrel = rsh->sh_type == SHT_REL;
            long esz = isrel ? 8 : 12;
            const struct insec *ts;
            const unsigned char *tbase;
            if (rsh->sh_type != SHT_REL && rsh->sh_type != SHT_RELA)
                continue;
            if (rsh->sh_info >= (Elf64_Word)o->nsh ||
                o->sec_out[rsh->sh_info] < 0)
                continue;
            ts = &l->insecs[o->sec_out[rsh->sh_info]];
            if (ts->discarded || ts->is_bss)
                continue;
            tbase = o->buf + sh_at(o, (int)rsh->sh_info)->sh_offset;
            for (long j = 0; j + esz <= (long)rsh->sh_size; j += esz) {
                const unsigned char *r = o->buf + rsh->sh_offset + j;
                Elf64_Addr r_offset = get32loc(r), S;
                unsigned info = get32loc(r + 4);
                unsigned type = info & 0xff, symi = info >> 8;
                const unsigned char *loc = tbase + r_offset;
                int arm, call, gsym, ok, other, cond_al, key;
                long long A, d;
                if (type != R_ARM_THM_CALL && type != R_ARM_THM_JUMP24 &&
                    type != R_ARM_CALL && type != R_ARM_JUMP24)
                    continue;
                if (symi >= (unsigned)o->nsym || r_offset + 4 > ts->size)
                    continue;
                arm = type == R_ARM_CALL || type == R_ARM_JUMP24;
                call = type == R_ARM_THM_CALL || type == R_ARM_CALL;
                A = isrel ? (arm ? read_arm_b24(loc) + 8
                                 : read_thm_b24(loc) + 4)
                          : (int)get32loc(r + 8);
                gsym = veneer_gsym(l, o, symi);
                S = veneer_target(l, o, symi, gsym, &ok);
                if (!ok)
                    continue;
                other = arm ? (int)(S & 1) : thumb_to_arm(l, o, symi, S);
                cond_al = !arm || (get32loc(loc) >> 28) == 0xe;
                d = (long long)(S & ~(Elf64_Addr)1) + A -
                    (long long)(ts->vaddr + r_offset + (arm ? 8 : 4));
                if (!veneer_needed(arm, call, cond_al, other, d))
                    continue;
                key = veneer_key(l, ts);
                if (veneer_find(l, key, gsym, oi, symi, A, arm) >= 0)
                    continue;
                LS_PUSH(l->vn, l->nvn, l->capvn);
                {
                    struct veneer *v = &l->vn[l->nvn++];
                    memset(v, 0, sizeof *v);
                    v->key = key;
                    v->gsym = gsym;
                    v->obj = oi;
                    v->symi = (int)symi;
                    v->arm = arm;
                    v->other = other;
                    v->addend = A;
                    v->sec = -1;
                    v->k = -1;
                }
                added++;
            }
        }
    }
    if (added)
        arm_veneer_place(l);
    return added;
}

/* The veneer for the branch in `ts` to the relocation's target, when it
 * has one: its address, or 0. */
static Elf64_Addr arm_veneer_addr(struct linker *l, const struct insec *ts,
                                  struct object *o, Elf64_Word symi,
                                  long long A, int arm)
{
    int oi, k;
    if (!ts || !l->nvn)
        return 0;
    for (oi = 0; oi < l->nobj && l->objs[oi] != o; oi++)
        ;
    k = veneer_find(l, veneer_key(l, ts), veneer_gsym(l, o, symi), oi, symi,
                    A, arm);
    if (k < 0)
        return 0;
    return l->insecs[l->vn[k].sec].vaddr + 12 * (Elf64_Addr)l->vn[k].k;
}

/* After the final layout and finalize_symbols: the veneers' bytes. */
static void arm_veneer_fill(struct linker *l)
{
    for (int k = 0; k < l->nvn; k++) {
        const struct veneer *v = &l->vn[k];
        unsigned char *p = (unsigned char *)l->insecs[v->sec].data + 12 * v->k;
        int uw;
        Elf64_Addr to = v->gsym >= 0
            ? l->syms[v->gsym].value
            : reloc_symval(l, l->objs[v->obj], (Elf64_Word)v->symi, &uw);
        unsigned dest;
        to += (Elf64_Addr)v->addend;
        /* bx and the pop into pc take the state from the low bit, which
         * a section symbol plus an offset does not carry: Thumb unless
         * the branch changes state to ARM, or stays there */
        if (v->arm ? !v->other : v->other)
            to &= ~(Elf64_Addr)1;
        else
            to |= 1;
        dest = (unsigned)to;
        if (v->arm) {
            put32(p, 0xe59fc000u);                  /* ldr ip, [pc] */
            put32(p + 4, 0xe12fff1cu);              /* bx ip */
            put32(p + 8, dest);
        } else if (l->lb_thumb1) {
            put16(p, 0xb403);                       /* push {r0, r1} */
            put16(p + 2, 0x4801);                   /* ldr r0, [pc, #4] */
            put16(p + 4, 0x9001);                   /* str r0, [sp, #4] */
            put16(p + 6, 0xbd01);                   /* pop {r0, pc} */
            put32(p + 8, dest);
        } else {
            for (int h = 0; h < 2; h++) {           /* movw, movt ip */
                unsigned imm = h ? dest >> 16 : dest & 0xffffu;
                put16(p + 4 * h, (h ? 0xf2c0u : 0xf240u) |
                                 ((imm >> 1) & 0x400u) | (imm >> 12));
                put16(p + 4 * h + 2, ((imm << 4) & 0x7000u) |
                                     (12u << 8) | (imm & 0xffu));
            }
            put16(p + 8, 0x4760);                   /* bx ip */
            put16(p + 10, 0xbf00);                  /* nop */
        }
    }
}

static struct object g_cmse_obj;

/* After layout: the veneers' bytes. */
static void cmse_fill(struct linker *l)
{
    g_cmse_obj.name = "the linker's CMSE veneers";
    if (l->sg_sec >= 0) {
        struct insec *s = &l->insecs[l->sg_sec];
        unsigned char *b = (unsigned char *)s->data;
        for (int k = 0; k < l->nsg; k++) {
            unsigned char *p = b + 8 * k;
            Elf64_Addr at = s->vaddr + 8 * (Elf64_Addr)k;
            Elf64_Addr to = l->syms[l->sg[k].se].value & ~(Elf64_Addr)1;
            put16(p, 0xe97f);
            put16(p + 2, 0xe97f);
            put16(p + 4, 0xf000);
            put16(p + 6, 0x9000);                   /* b.w */
            patch_thm_b24(&g_cmse_obj, p + 4,
                          (long long)to - (long long)(at + 8));
        }
    }
    arm_veneer_fill(l);
}

/* --out-implib: the import library, an ELF32 relocatable holding one
 * absolute global function symbol per veneer. */
static void write_implib(struct linker *l, const char *path)
{
    long nstr = 1, symoff = 52, strsz, shstroff, shoff, len;
    static const char shstr[] = "\0.symtab\0.strtab\0.shstrtab";
    unsigned char *img;
    FILE *f;
    for (int k = 0; k < l->nsg; k++)
        nstr += (long)strlen(l->syms[l->sg[k].sym].name) + 1;
    strsz = nstr;
    shstroff = symoff + 16L * (l->nsg + 1) + strsz;
    shoff = (shstroff + (long)sizeof shstr + 3) & ~3L;
    len = shoff + 4 * 40;
    img = xcalloc((size_t)len, 1);
    memcpy(img, "\177ELF", 4);
    img[4] = ELFCLASS32; img[5] = ELFDATA2LSB; img[6] = EV_CURRENT;
    put16(img + 16, ET_REL);
    put16(img + 18, EM_ARM);
    put32(img + 20, EV_CURRENT);
    put32(img + 32, (unsigned)shoff);
    put32(img + 36, EF_ARM_EABI_VER5);
    put16(img + 40, 52);
    put16(img + 46, 40);
    put16(img + 48, 4);
    put16(img + 50, 3);
    {
        long so = 1;
        unsigned char *strs = img + symoff + 16L * (l->nsg + 1);
        for (int k = 0; k < l->nsg; k++) {
            const struct symbol *g = &l->syms[l->sg[k].sym];
            unsigned char *y = img + symoff + 16L * (k + 1);
            size_t nl = strlen(g->name);
            memcpy(strs + so, g->name, nl);
            put32(y, (unsigned)so);
            put32(y + 4, (unsigned)g->value);
            put32(y + 8, 8);
            y[12] = (unsigned char)((STB_GLOBAL << 4) | STT_FUNC);
            put16(y + 14, SHN_ABS);
            so += (long)nl + 1;
        }
    }
    memcpy(img + shstroff, shstr, sizeof shstr);
    {
        /* null; .symtab; .strtab; .shstrtab */
        unsigned char *h = img + shoff + 40;
        put32(h, 1); put32(h + 4, SHT_SYMTAB);
        put32(h + 16, (unsigned)symoff);
        put32(h + 20, (unsigned)(16L * (l->nsg + 1)));
        put32(h + 24, 2); put32(h + 28, 1);
        put32(h + 32, 4); put32(h + 36, 16);
        h += 40;
        put32(h, 9); put32(h + 4, SHT_STRTAB);
        put32(h + 16, (unsigned)(symoff + 16L * (l->nsg + 1)));
        put32(h + 20, (unsigned)strsz); put32(h + 32, 1);
        h += 40;
        put32(h, 17); put32(h + 4, SHT_STRTAB);
        put32(h + 16, (unsigned)shstroff);
        put32(h + 20, (unsigned)sizeof shstr); put32(h + 32, 1);
    }
    f = fopen(path, "wb");
    if (!f || fwrite(img, 1, (size_t)len, f) != (size_t)len || fclose(f))
        die("cannot write the import library %s", path);
    free(img);
}

int embld_link(const char **inputs, int ninputs, const char *out,
               const struct link_opts *opts)
{
    struct linker l;
    memset(&l, 0, sizeof l);
    /* Carry DWARF into the image. A build without -g has no .debug_*
     * to collect, so this costs nothing there; with -g it is the
     * difference between an executable gdb can open and one it cannot. */
    l.keep_debug = 1;
    l.base = (opts && (opts->base || opts->have_base)) ? opts->base
                                                       : DEFAULT_BASE;
    l.entry = (opts && opts->entry) ? opts->entry : "_start";
    l.lma_offset = (opts) ? opts->lma_offset : 0;
    l.data_base = (opts) ? opts->data_base : 0;
    l.rom_limit = (opts) ? opts->rom_limit : 0;
    l.stack_top = (opts && opts->have_stack) ? opts->stack_top : 0;
    l.stub_sec = -1;
    l.gc_sections = opts && opts->gc_sections;
    l.print_gc = opts && opts->print_gc_sections;
    if (opts) {
        l.gc_undefs = opts->undefs;
        l.gc_nundefs = opts->nundefs;
    }

    /* -T: the script is read before any input, because it can name
     * inputs of its own and symbols an archive has to supply. */
    struct ls_script *sc = NULL;
    const char **all = inputs;
    int nall = ninputs;
    if (opts && opts->script) {
        if (opts->have_base || opts->data_base || opts->rom_limit ||
            opts->lma_offset || opts->have_stack || opts->emit_embx)
            die("-T %s lays the image out, so -Ttext, -Tdata, -Tstack, "
                "--rom-limit, --lma-offset and --embx cannot be given with "
                "it", opts->script);
        sc = ls_parse_file(opts->script);
        l.sc = sc;
        for (int k = 0; k < opts->nlibdirs; k++)
            lp_push_str(&sc->dirs, &sc->ndirs, &sc->capdirs, opts->libdirs[k]);
        lp_push_str(&sc->dirs, &sc->ndirs, &sc->capdirs,
                    ls_dirname(opts->script));
        if (!opts->entry && sc->entry)
            l.entry = sc->entry;
        /* An entry the link was told about is a reference: the archive
         * member that defines Reset_Handler is pulled for it, as ld does. */
        if (opts->entry || sc->entry)
            sym_intern(&l, l.entry);
        for (int k = 0; k < sc->nextern; k++)
            sym_intern(&l, sc->externs[k]);
        if (sc->ninputs) {
            all = xmalloc((size_t)(ninputs + sc->ninputs) * sizeof *all);
            for (int k = 0; k < ninputs; k++)
                all[k] = inputs[k];
            for (int k = 0; k < sc->ninputs; k++)
                all[nall++] = ls_find_input(sc->inputs[k], sc->dirs, sc->ndirs);
        }
    }
    for (int k = 0; opts && k < opts->nundefs; k++)
        sym_intern(&l, opts->undefs[k]);

    /* Explicit objects are always linked; archives are stashed and their
     * members pulled on demand (left-to-right, as a linker does — so an
     * archive satisfies references that appear before it on the line). */
    for (int i = 0; i < nall; i++) {
        long len;
        unsigned char *buf = read_file(all[i], &len);
        if (is_archive(buf, len)) {
            parse_archive(&l, all[i], buf, len);
            pull_archives(&l); /* satisfy what is undefined so far */
        } else {
            add_object(&l, parse_object(all[i], buf, len));
        }
    }
    /* A final fixed-point pass, so a later object's references can still
     * reach back into an earlier archive (the --start-group behaviour,
     * always on: correct over order-sensitive). */
    pull_archives(&l);
    /* every input is in: the CMSE veneers, before any layout */
    cmse_scan(&l);
    arm_veneer_init(&l);
    if (opts && opts->out_implib && !opts->cmse_implib)
        die("--out-implib needs --cmse-implib: the import library this "
            "linker writes is ARMv8-M's secure gateway one");
    if (l.print_gc && !l.gc_sections)
        die("--print-gc-sections without --gc-sections: nothing is "
            "collected to print");

    if (sc) {
        ls_layout(&l, sc, opts->orphan_mode);
        finalize_symbols(&l);
        cmse_fill(&l);
        /* ld's bounds for an output section whose name is a C
         * identifier, which is how a program walks a table it put in a
         * section of its own */
        for (int k = 0; k < sc->nosec; k++) {
            const struct ls_osec *o = &sc->osecs[k];
            char sym[256];
            if (o->discard || !o->laid || !is_c_ident(o->name) ||
                strlen(o->name) > 200)
                continue;
            snprintf(sym, sizeof sym, "__start_%s", o->name);
            define_linker_symbol(&l, xstrndup(sym, strlen(sym)),
                                 (Elf64_Addr)o->vma);
            snprintf(sym, sizeof sym, "__stop_%s", o->name);
            define_linker_symbol(&l, xstrndup(sym, strlen(sym)),
                                 (Elf64_Addr)(o->vma + o->size));
        }
        arm_attrs_check(&l);
        Elf64_Addr ev = 0;
        struct symbol *e = sym_find(&l, l.entry);
        if (e && e->defined) {
            ev = e->value;
        } else if (opts->entry || sc->entry) {
            die("entry symbol '%s' is undefined", l.entry);
        } else {
            /* ld's default: no ENTRY and no _start is a warning, and the
             * entry is where the code starts. A Cortex-M ignores it (the
             * vector table says where to start); a loader may not. */
            for (int c = 0; c < sc->ncmd && !ev; c++)
                if (sc->cmds[c].kind == LS_OSEC) {
                    const struct ls_osec *o = &sc->osecs[sc->cmds[c].osec];
                    if (!o->discard && o->laid && o->exec && o->size)
                        ev = (Elf64_Addr)o->vma;
                }
            fprintf(stderr, "embld: warning: no ENTRY in the script and no "
                    "_start; the entry is 0x%llx, where the code starts\n",
                    (unsigned long long)ev);
        }
        for (int i = 0; i < l.nobj; i++)
            apply_relocs(&l, l.objs[i]);
        if (opts->map_file)
            write_map(&l, sc, NULL, opts->map_file);
        if (opts->print_memory_usage)
            print_memory_usage(sc);
        write_exec_script(&l, sc, out, ev);
        emit_embdbg(&l, out);
        if (opts->out_implib)
            write_implib(&l, opts->out_implib);
        return 0;
    }

    /* The entry stub goes in before layout so it gets an address like
     * any other section, and is FILLED after, when the entry symbol has
     * one. Refused on a machine that does not need it rather than
     * silently ignored: -Tstack on ARM would mean the caller believes
     * something about the image that is not true. */
    /* RX: a C function `_start` is the object's `__start`; an entry
     * named the C way is found under that spelling. */
    if (l.machine == EM_RX && !sym_find(&l, l.entry)) {
        char *un = xmalloc(strlen(l.entry) + 2);
        un[0] = '_';
        memcpy(un + 1, l.entry, strlen(l.entry) + 1);
        if (sym_find(&l, un))
            l.entry = un;
        else
            free(un);
    }
    if (l.gc_sections)
        gc_sections(&l, l.gc_undefs, l.gc_nundefs);
    if (opts && opts->have_stack) {
        /* ARM: for an A-profile core (armv7a-none-eabi), which starts in
         * ARM state with no stack; the stub is A32. A Cortex-M image
         * reads its sp from the vector table and wants none -- and could
         * not run one, having no ARM state. */
        if (l.machine != EM_RISCV && l.machine != EM_MIPS &&
            l.machine != EM_LOONGARCH && l.machine != EM_ARM &&
            l.machine != EM_TRICORE && l.machine != EM_XTENSA &&
            l.machine != EM_PPC && l.machine != EM_RX &&
            l.machine != EM_SPARC && l.machine != EM_68K)
            die("-Tstack is a RISC-V, MIPS, LoongArch, ARMv7-A, TriCore, Xtensa, "
                "PowerPC, RX, SPARC and ColdFire "
                "option: "
                "every other target here starts with a stack pointer already "
                "set (a Cortex-M reads its own from the vector table)");
        if (l.machine == EM_TRICORE) {
            if (!opts->have_csa)
                die("-Tstack on TriCore needs --csa START:END as well: the "
                    "stub links the context-save areas every CALL takes, "
                    "and with none the entry's first call traps");
            if ((opts->csa_start & 63) || (opts->csa_end & 63) ||
                opts->csa_end < opts->csa_start + 128 ||
                (opts->csa_start >> 28) != ((opts->csa_end - 1) >> 28) ||
                ((opts->csa_start | (opts->csa_end - 1)) & 0x0fc00000UL))
                die("--csa 0x%lx:0x%lx: the context-save areas must be "
                    "64-byte aligned, at least two of them, and within the "
                    "first 4 MiB of one 256 MiB segment (a link word holds "
                    "address bits 31:28 and 21:6)",
                    opts->csa_start, opts->csa_end);
            l.csa_start = opts->csa_start;
            l.csa_end = opts->csa_end;
        }
        add_entry_stub(&l);
    }

    Elf64_Addr text_start, data_start;
    Elf64_Xword text_size, data_filesz, data_memsz;
    struct osec_bound bounds[OSEC_COUNT + MAX_ORPHANS];
    memset(bounds, 0, sizeof bounds);
    Elf64_Addr tls_start = 0;
    Elf64_Xword tls_filesz = 0, tls_memsz = 0, tls_align = 1;
    layout(&l, bounds, &text_start, &text_size, &data_start, &data_filesz,
           &data_memsz, &tls_start, &tls_filesz, &tls_memsz, &tls_align);
    /* The ARM veneers these addresses call for move what follows them:
     * laid out again, until no branch wants a new one. */
    for (int round = 0; arm_veneer_scan(&l); round++) {
        if (round == VENEER_MAX_ROUNDS)
            die("the long-branch veneers do not settle: each round's put "
                "another branch out of reach");
        layout(&l, bounds, &text_start, &text_size, &data_start,
               &data_filesz, &data_memsz, &tls_start, &tls_filesz,
               &tls_memsz, &tls_align);
    }
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
    cmse_fill(&l);
    define_brackets(&l, bounds);
    define_orphan_brackets(&l, bounds);
    define_end_symbols(&l, data_start + data_memsz);   /* L1: kernel_end/_end */
    define_firmware_symbols(&l, data_start, data_filesz, data_memsz);

    /* Every input is loaded by now, which is what this question needs:
     * whether the SET of objects can be linked at all. See
     * arm_attrs_check. */
    arm_attrs_check(&l);

    struct symbol *e = sym_find(&l, l.entry);
    if (!e || !e->defined)
        die("entry symbol '%s' is undefined", l.entry);

    if (l.stub_sec >= 0)
        fill_entry_stub(&l, e->value);

    for (int i = 0; i < l.nobj; i++)
        apply_relocs(&l, l.objs[i]);
    if (opts && opts->map_file)
        write_map(&l, NULL, bounds, opts->map_file);
    if (opts && opts->print_memory_usage)
        print_memory_usage(NULL);

    if (opts && opts->emit_embx) {
        emit_embx(&l, out, opts->caps, e->value, text_start, text_size,
                  data_start, data_filesz, data_memsz);
    } else {
        write_exec(&l, out,
                   l.stub_sec >= 0 ? l.insecs[l.stub_sec].vaddr : e->value,
                   text_start, text_size,
                   data_start, data_filesz, data_memsz,
                   tls_start, tls_filesz, tls_memsz, tls_align);
        emit_embdbg(&l, out);   /* a .embdbg sidecar if any input carries -g info */
    }
    if (opts && opts->out_implib)
        write_implib(&l, opts->out_implib);
    return 0;
}
