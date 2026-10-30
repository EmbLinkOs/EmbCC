/* `embcc -S` — the assembly the compiler just emitted, as text.
 *
 * ---- why it is written this way ----
 *
 * The obvious implementation is a second emitter: every place the backend
 * writes bytes, also write a mnemonic. That is a parallel implementation of
 * the same knowledge (R1), and the two drift — the text says one thing, the
 * object contains another, and the bug is invisible until someone
 * reassembles the text and gets a different program.
 *
 * So `-S` is produced from the BYTES the backend actually emitted, by the
 * disassembler EmbDBG already has, with the relocation sites the backend
 * already recorded rendered back into symbol names. There is one instruction
 * encoder and one decoder, and the test reassembles the output and compares
 * it with the object byte for byte — so the text cannot claim something the
 * object does not contain.
 *
 * That the decoder is complete enough for this is not an assumption: EmbCC's
 * own optimizer, 8000 instructions of it, disassembles with no undecoded
 * bytes.
 *
 * ---- what it is not ----
 *
 * A disassembly. Only x86-64 has a decoder here, so only its text carries
 * mnemonics, as comments; every other target's is its bytes with each
 * relocation named, grouped by the target's length rule (RX has none, and
 * its bytes go out in runs), and MIPS writes a relocated instruction
 * symbolically. What cannot be spelt is refused rather than emitted wrong
 * (docs/internals/contributing.md: unsupported must fail, never be
 * silently wrong). tests/golden/asmout-roundtrip.sh reassembles every
 * target's text -- with EmbCC's own assembler, and llvm-mc where it has
 * the target -- and compares it with the object.
 */
#include "asmout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#include "../arch/backend.h"
#include "../arch/target.h"
#if FAMILY_BUILT(arm)
#include "../arch/thumb/attrs.h"
#endif
#include "../elf/elf.h"
#include "../ir/ir.h"
#include "../parse/ast.h"
#include "../sema/type.h"
#include "../../tools/embdbg/embdbg_core.h"

/* A symbol as an assembler reads it. C lets an identifier hold UTF-8
 * letters (`größe`), and a name with any byte outside [A-Za-z0-9_.$] is
 * written in quotes, as clang writes it; the object file carries the
 * bytes either way. The quoted copy lives as long as the compile. */
/* A pointer-sized slot holding an address: as wide as the target's
 * pointer -- `.quad` was written everywhere, which a 32-bit target's
 * assembler rejects and a 64-bit one would lay out twice too wide -- and
 * on AVR a FUNCTION's address is its word address, pm(), as the object
 * writer's R_AVR_16_PM says. */
static void ptr_slot(struct outbuf *b, const char *sym, long addend, int fn)
{
    int ps = target_ptr_size();
    const char *dir = ps == 8 ? ".quad" : ps == 4 ? ".long" : ".short";
    if (ps == 2 && fn)
        ob_fmt(b, "\t%s\tpm(%s", dir, sym);
    else
        ob_fmt(b, "\t%s\t%s", dir, sym);
    if (addend)
        ob_fmt(b, "%+ld", addend);
    ob_str(b, ps == 2 && fn ? ")\n" : "\n");
}

static const char *asym(const char *n)
{
    int plain = 1;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' ||
              *p == '$'))
            plain = 0;
    if (plain)
        return n;
    size_t len = strlen(n);
    char *q = xmalloc(len + 3);
    q[0] = '"';
    memcpy(q + 1, n, len);
    q[len + 1] = '"';
    q[len + 2] = 0;
    return q;
}

/* A C name as the object spells it, which is what an assembler must be
 * given: RX's ELF symbols carry the underscore its ABI prefixes
 * (src/elf/write.c), and `int sw()` is `_sw` in RX assembly. Assembler
 * names -- an asm block's labels, a section -- are already spelt so. */
static const char *csym(const char *n)
{
    /* (target.c's, not the backend table's: tools/embls links this file
     * and no code generator) */
    const char *p = target_sym_prefix();
    /* (not on an assembler-local name or a static's `.`-joined one, as
     * the writer leaves those: `main.z`, `.Lcomplit.0`) */
    if (n[0] == '\001')               /* an asm label: the name as given */
        return asym(n + 1);
    if (!p || !*p || n[0] == '.' || n[0] == '$')
        return asym(n);
    size_t pl = strlen(p), nl = strlen(n);
    char *q = xmalloc(pl + nl + 1);
    memcpy(q, p, pl);
    memcpy(q + pl, n, nl + 1);
    return asym(q);
}

/* One place in .text that names a symbol. Collected from what the backend
 * recorded, so this is the same information the ELF writer turns into
 * relocations — not a second derivation of it. */
struct site {
    long addend;             /* a function site's offset into its target */
    int off;                 /* the relocated field's offset in .text */
    const char *name;        /* the symbol, or NULL for a .rodata string */
    int str_off;             /* when name is NULL: offset inside .rodata */
    int kind;                /* enum reloc_kind */
    int done;                /* -S wrote its .reloc */
    int labeled;             /* a text site: its target's label is written */
};

static int site_cmp(const void *a, const void *b)
{
    int x = ((const struct site *)a)->off, y = ((const struct site *)b)->off;
    return x < y ? -1 : x > y;
}

/* The site whose field falls inside [lo, hi), or NULL. */
static struct site *site_in(struct site *s, int n, int lo, int hi)
{
    for (int i = 0; i < n; i++)
        if (s[i].off >= lo && s[i].off < hi)
            return &s[i];
    return NULL;
}

/* The file-scope asm blocks and naked functions, with their labels and
 * relocations (at the end of this file). */
static int blocks_by_reloc(void);
static void blocks_emit(struct outbuf *b, struct unit *u,
                        const unsigned char *text, long from, long textlen);

/* `.LC<n>` for the string at that .rodata offset — the label the data
 * section below defines. */
static void str_label(char *out, size_t cap, const struct ir_unit *iu, int off)
{
    for (int i = 0; i < iu->nstrs; i++)
        if (iu->strs[i].off == off) {
            snprintf(out, cap, ".LC%d", i);
            return;
        }
    snprintf(out, cap, ".LC_at_%d", off);
}

/* A relocation against a place in this object's own code -- an Xtensa
 * literal-pool word holding a label's address, an AVR `jmp` too far for
 * rjmp -- which the object makes against .text's section symbol plus the
 * offset. An assembler is given a local label at that offset, which it
 * relocates the same way. */
static int text_site(int kind)
{
    return kind == RK_XTENSA_TEXT32 || kind == RK_AVR_TEXT_CALL;
}

static void text_label(char *out, size_t cap, int off)
{
    snprintf(out, cap, ".Ltext%d", off);
}

/* Define every text label that falls at pc, once each (sites are in .text
 * order, so the first site naming an offset is the one that defines it).
 * A target no instruction starts at is caught after the functions. */
static void text_labels(struct outbuf *b, struct site *s, int n, long pc)
{
    for (int i = 0; i < n; i++) {
        if (!text_site(s[i].kind) || s[i].str_off != pc)
            continue;
        s[i].labeled = 1;
        int first = 1;
        for (int j = 0; j < i; j++)
            if (text_site(s[j].kind) && s[j].str_off == s[i].str_off)
                first = 0;
        if (first) {
            char lab[32];
            text_label(lab, sizeof lab, s[i].str_off);
            ob_fmt(b, "%s:\n", lab);
        }
    }
}

/* A line of bytes holds at most one relocated field, and starts at every
 * text label: shorten [pc, pc + len) to end where a second field or a
 * label begins. A grouping that is exact never needs it; a coarse one
 * (RX's runs, a pool word beside code) would otherwise lose the second
 * relocation, or a label, without a word. */
static int cut_group(const struct site *s, int n, long pc, int len)
{
    long first = -1;
    for (int i = 0; i < n; i++) {
        long at = s[i].off;
        if (at >= pc && at < pc + len) {
            if (first >= 0 && at > first)
                len = (int)(at - pc);
            else
                first = at;
        }
        if (text_site(s[i].kind) && s[i].str_off > pc &&
            s[i].str_off < pc + len)
            len = (int)(s[i].str_off - pc);
    }
    return len;
}

static const char *type_sigil(void);

/* The build attributes the object carries (src/arch/backends.c's
 * elf_notes), as the directives that make them: ARM's .eabi_attribute,
 * RISC-V's .attribute. Without them another assembler writes none, and a
 * linker has nothing to refuse a soft-float object against a hard-float
 * one with (Tag_ABI_VFP_args). Decoded from the bytes the object gets,
 * so the two cannot disagree. */
static unsigned long attr_uleb(const unsigned char *a, size_t n, size_t *i)
{
    unsigned long v = 0;
    int sh = 0;
    while (*i < n) {
        unsigned char c = a[(*i)++];
        v |= (unsigned long)(c & 0x7f) << sh;
        sh += 7;
        if (!(c & 0x80))
            break;
    }
    return v;
}

static void attrs_emit(struct outbuf *b)
{
    unsigned char *a = NULL;
    size_t n = 0;
    const char *dir = NULL;
    int arm = 0;
#if FAMILY_BUILT(arm)
    if (target_get() == TARGET_THUMB) {
        a = arm_build_attributes(&n);
        dir = ".eabi_attribute";
        arm = 1;
    }
#endif
#if FAMILY_BUILT(riscv)
    if (target_get() == TARGET_RISCV32 || target_get() == TARGET_RISCV64) {
        a = riscv_build_attributes(&n);
        dir = ".attribute";
    }
#endif
    if (!a)
        return;
    /* 'A', then vendor subsections (length, name), each holding
     * sub-subsections (tag, length); only Tag_File's pairs are written */
    size_t i = 1;
    while (a[0] == 'A' && i + 4 <= n) {
        size_t sub = (size_t)target_get_uint(a + i, 4);
        size_t end = i + sub, j = i + 4;
        if (sub < 5 || end > n)
            break;
        while (j < end && a[j])
            j++;
        j++;                                 /* the vendor's name */
        while (j + 5 <= end) {
            size_t at = j;
            unsigned long tag = attr_uleb(a, end, &j);
            size_t sz = (size_t)target_get_uint(a + j, 4);
            size_t send = at + sz;
            j += 4;
            if (sz < 5 || send > end)
                break;
            while (tag == 1 && j < send) {
                unsigned long t = attr_uleb(a, send, &j);
                /* a string: ARM's CPU names and, past 32, an odd tag
                 * (the ABI's rule); RISC-V's every odd tag */
                int str = arm ? t == 4 || t == 5 || (t > 32 && (t & 1))
                              : (t & 1);
                if (str) {
                    ob_fmt(b, "\t%s\t%lu, \"", dir, t);
                    while (j < send && a[j])
                        ob_fmt(b, "%c", a[j++]);
                    j++;
                    ob_str(b, "\"\n");
                } else {
                    ob_fmt(b, "\t%s\t%lu, %lu\n", dir, t,
                           attr_uleb(a, send, &j));
                }
            }
            j = send;
        }
        i = end;
    }
    free(a);
}

/* A file-scope asm block defines the name (its own .globl or .weak
 * says how it binds). */
static int topasm_defines(const struct unit *u, const char *name)
{
    for (const struct topasm *ta = u->topasm; ta; ta = ta->next)
        for (int k = 0; k < ta->nsyms; k++)
            if (!strcmp(ta->syms[k].name, name))
                return 1;
    return 0;
}

/* Which globals a data section holds, as main.c placed them. */
enum gsec { GS_DATA, GS_BSS, GS_RODATA, GS_TDATA, GS_TBSS, GS_NAMED };

static int gsec_holds(const struct global *g, enum gsec k, int named)
{
    if (g->absorbed || !g->defined || g->is_common)
        return 0;
    switch (k) {
    case GS_DATA:   return !g->in_bss && !g->in_rodata && !g->is_tls && !g->named;
    case GS_BSS:    return g->in_bss && !g->is_tls && !g->named;
    case GS_RODATA: return g->in_rodata;
    case GS_TDATA:  return g->is_tls && !g->in_bss;
    case GS_TBSS:   return g->is_tls && g->in_bss;
    case GS_NAMED:  return g->named == named;
    }
    return 0;
}

/* One data section's objects, each at the offset main.c gave it: the gap
 * before it as zeros, then its bytes -- a pointer slot as the address it
 * holds (ptr_slot), the rest as they are. `cur` is where the section
 * already ends in the text (.rodata's strings), or -1 for nothing yet. */
static void globals_emit(struct outbuf *b, struct unit *u,
                         const struct ir_unit *iu, enum gsec k, int named,
                         const char *name, long cur)
{
    int open = 0;
    if (cur < 0)
        cur = 0;
    for (struct global *g = u->globals; g; g = g->next) {
        if (!gsec_holds(g, k, named))
            continue;
        int sz = global_size(g);
        int nobits = g->in_bss;
        if (!open) {
            switch (k) {
            case GS_DATA:   ob_str(b, "\n\t.data\n"); break;
            case GS_BSS:    ob_str(b, "\n\t.bss\n"); break;
            case GS_RODATA: ob_str(b, "\n\t.section\t.rodata\n"); break;
            case GS_TDATA:
                ob_fmt(b, "\n\t.section\t.tdata,\"awT\",%sprogbits\n",
                       type_sigil());
                break;
            case GS_TBSS:
                ob_fmt(b, "\n\t.section\t.tbss,\"awT\",%snobits\n",
                       type_sigil());
                break;
            case GS_NAMED:
                ob_fmt(b, "\n\t.section\t%s,\"a%s%s\",%s%s\n", asym(name),
                       g->sec_flags & SHF_WRITE ? "w" : "",
                       g->sec_flags & SHF_EXECINSTR ? "x" : "", type_sigil(),
                       nobits ? "nobits" : "progbits");
                break;
            }
            open = 1;
        }
        /* the alignment as main.c computed it: the section claims the
         * strictest, and the zeros below put the object where it was */
        int al = g->ty ? ty_align(g->ty) : 1;
        if (g->user_align > al)
            al = g->user_align;
        al = target_object_align(g->ty && g->ty->kind == TY_ARRAY, sz, al);
        if (g->off < cur)
            internal_error("-S: '%s' at offset %d overlaps the object "
                           "before it", g->name, g->off);
        if (g->off > cur)
            ob_fmt(b, "\t.zero\t%ld\n", g->off - cur);
        if (!g->is_static)
            ob_fmt(b, "\t.%s\t%s\n", g->is_weak ? "weak" : "globl",
                   csym(g->name));
        ob_fmt(b, "\t.balign\t%d\n\t.type\t%s, %s%s\n%s:\n", al,
               csym(g->name), type_sigil(),
               g->is_tls ? "tls_object" : "object", csym(g->name));
        /* an empty struct still takes a byte, as in the object */
        int span = sz ? sz : 1;
        unsigned char scalar[16];
        const char *img = g->init_bytes;
        int ilen = g->init_len;
        if (!img && g->has_init && !nobits && g->ty &&
            ty_size(g->ty) <= (int)sizeof scalar) {
            memset(scalar, 0, sizeof scalar);
            target_put_uint(scalar, ty_size(g->ty),
                            (unsigned long)g->init);
            img = (const char *)scalar;
            ilen = ty_size(g->ty);
        }
        if (nobits || !img) {
            ob_fmt(b, "\t.zero\t%d\n", span);
        } else {
            int at = 0;
            while (at < span) {
                const struct greloc *r = NULL;
                for (int q = 0; q < g->nrelocs; q++)
                    if (g->relocs[q].off == at) { r = &g->relocs[q]; break; }
                if (r) {
                    char sym[160];
                    if (r->str)
                        str_label(sym, sizeof sym, iu, r->str_off);
                    else if (r->gtarget)
                        snprintf(sym, sizeof sym, "%s", csym(r->gtarget->name));
                    else if (r->ftarget)
                        snprintf(sym, sizeof sym, "%s", csym(r->ftarget->name));
                    else
                        snprintf(sym, sizeof sym, "0");
                    ptr_slot(b, sym, r->addend, r->ftarget != NULL);
                    at += target_ptr_size();
                    continue;
                }
                /* bytes up to the next relocation */
                int stop = span;
                for (int q = 0; q < g->nrelocs; q++)
                    if (g->relocs[q].off > at && g->relocs[q].off < stop)
                        stop = g->relocs[q].off;
                ob_str(b, "\t.byte\t");
                for (int j = at; j < stop; j++)
                    ob_fmt(b, "%s%d", j > at ? "," : "",
                           j < ilen ? (int)(unsigned char)img[j] : 0);
                ob_str(b, "\n");
                at = stop;
            }
        }
        ob_fmt(b, "\t.size\t%s, %d\n", csym(g->name), sz);
        cur = g->off + span;
    }
}

/* The ELF relocation an assembler directive must name, and the addend the
 * backend's zeroed field implies. A PC-relative field is measured from its
 * own END, so the addend is -(bytes after the field), which for every form
 * EmbCC emits is -4. */
/* (kind, target) -> the ELF relocation type name an assembler accepts.
 *
 * This used to answer R_X86_64_* for every target, which was harmless
 * only because -S refused aarch64 and had not been taught about the
 * three machines added after it. The kinds are already per-machine
 * (target.h): what was missing was the other half of the mapping that
 * src/elf/write.c has always had.
 *
 * NULL means "this kind cannot be spelled as a plain .reloc on this
 * target", and the caller refuses rather than emitting a relocation
 * that assembles into the wrong program. Nothing lands there today;
 * the kind that nearly did is RISC-V's paired low half, whose operand
 * is the ADDRESS OF THE PAIRED auipc rather than the symbol, and which
 * is handled by labelling the auipc at its emission site. */
/* ARM assemblers treat '@' as the start of a comment, so a symbol type
 * is spelled `%function` there and `@function` everywhere else. Getting
 * this wrong is not subtle -- the whole directive vanishes into a
 * comment and the symbol is left untyped. */
/* Thumb state: every ARM target but ARMv7-A, which is A32 (ARM state). */
static int arm_thumb(void)
{
    return target_get() == TARGET_THUMB && !target_arm_a32();
}

static const char *type_sigil(void)
{
    return target_get() == TARGET_THUMB ? "%" : "@";
}

static const char *reloc_name(int kind)
{
    if (kind == RK_TAIL && target_get() != TARGET_THUMB &&
        target_get() != TARGET_AARCH64)
        kind = RK_CALL;       /* Thumb and aarch64 spell a branch apart */
    switch (target_get()) {
    case TARGET_AARCH64:
        switch (kind) {
        case RK_CALL:        return "R_AARCH64_CALL26";
        case RK_TAIL:        return "R_AARCH64_JUMP26";
        case RK_ADR_HI21:    return "R_AARCH64_ADR_PREL_PG_HI21";
        case RK_ADD_LO12:    return "R_AARCH64_ADD_ABS_LO12_NC";
        case RK_GOT_PAGE:    return "R_AARCH64_ADR_GOT_PAGE";
        case RK_GOT_LO12:    return "R_AARCH64_LD64_GOT_LO12_NC";
        case RK_TPREL_HI12:  return "R_AARCH64_TLSLE_ADD_TPREL_HI12";
        case RK_TPREL_LO12:  return "R_AARCH64_TLSLE_ADD_TPREL_LO12_NC";
        case RK_ABS64:       return "R_AARCH64_ABS64";
        case RK_ABS32:       return "R_AARCH64_ABS32";
        case RK_DATA_PREL32: return "R_AARCH64_PREL32";
        default:             return NULL;
        }
    case TARGET_THUMB:
        if (!arm_thumb())
            switch (kind) {              /* ARM state: the A32 fields */
            case RK_CALL:        return "R_ARM_CALL";
            case RK_TAIL:        return "R_ARM_JUMP24";
            case RK_THM_MOVW:    return "R_ARM_MOVW_ABS_NC";
            case RK_THM_MOVT:    return "R_ARM_MOVT_ABS";
            case RK_ABS32:       return "R_ARM_ABS32";
            case RK_DATA_PREL32: return "R_ARM_REL32";
            default:             return NULL;
            }
        switch (kind) {
        case RK_CALL:        return "R_ARM_THM_CALL";
        case RK_TAIL:        return "R_ARM_THM_JUMP24";
        case RK_THM_MOVW:    return "R_ARM_THM_MOVW_ABS_NC";
        case RK_THM_MOVT:    return "R_ARM_THM_MOVT_ABS";
        case RK_ABS32:       return "R_ARM_ABS32";
        case RK_DATA_PREL32: return "R_ARM_REL32";
        default:             return NULL;
        }
    case TARGET_RISCV32:
    case TARGET_RISCV64:
        switch (kind) {
        case RK_CALL:             return "R_RISCV_CALL_PLT";  /* as the object (target.c) */
        case RK_RISCV_PCREL_HI20: return "R_RISCV_PCREL_HI20";
        case RK_ABS64:            return "R_RISCV_64";
        case RK_ABS32:            return "R_RISCV_32";
        case RK_DATA_PREL32:      return "R_RISCV_32_PCREL";
        case RK_RISCV_PCREL_LO12_I: return "R_RISCV_PCREL_LO12_I";
        default:                  return NULL;
        }
    case TARGET_LOONGARCH64:
        switch (kind) {
        case RK_CALL:          return "R_LARCH_B26";   /* bl and b alike */
        case RK_LA_PCALA_HI20: return "R_LARCH_PCALA_HI20";
        case RK_LA_PCALA_LO12: return "R_LARCH_PCALA_LO12";
        case RK_ABS64:         return "R_LARCH_64";
        case RK_ABS32:         return "R_LARCH_32";
        case RK_DATA_PREL32:   return "R_LARCH_32_PCREL";
        default:               return NULL;
        }
    case TARGET_MIPS32:
    case TARGET_MIPS64:
        switch (kind) {
        case RK_MIPS_HIGHEST: return "R_MIPS_HIGHEST";
        case RK_MIPS_HIGHER: return "R_MIPS_HIGHER";
        case RK_ABS64:       return "R_MIPS_64";
        case RK_CALL:        return "R_MIPS_26";       /* jal and j alike */
        case RK_MIPS_TEXT26: return "R_MIPS_26";
        case RK_MIPS_HI16:   return "R_MIPS_HI16";
        case RK_MIPS_LO16:   return "R_MIPS_LO16";
        case RK_ABS32:       return "R_MIPS_32";
        default:             return NULL;
        }
    case TARGET_TRICORE:
        switch (kind) {
        case RK_CALL:        return "R_TRICORE_24REL";  /* call and j alike */
        case RK_TRICORE_HI:  return "R_TRICORE_HIADJ";
        case RK_TRICORE_LO:  return "R_TRICORE_LO";
        case RK_TRICORE_LO2: return "R_TRICORE_LO2";
        case RK_ABS32:       return "R_TRICORE_32ABS";
        default:             return NULL;
        }
    case TARGET_XTENSA:
        switch (kind) {
        case RK_CALL:          return "R_XTENSA_SLOT0_OP";  /* call8 */
        case RK_ABS32:         return "R_XTENSA_32";
        case RK_XTENSA_TEXT32: return "R_XTENSA_32";        /* a pool word */
        default:               return NULL;
        }
    case TARGET_RX:
        switch (kind) {
        case RK_CALL:        return "R_RX_DIR24S_PCREL";    /* bsr.a and bra.a */
        case RK_ABS32:       return "R_RX_DIR32";
        default:             return NULL;
        }
    case TARGET_PPC32:
        switch (kind) {
        case RK_CALL:          return "R_PPC_REL24";      /* bl and b alike */
        case RK_PPC_ADDR16_HA: return "R_PPC_ADDR16_HA";
        case RK_PPC_ADDR16_LO: return "R_PPC_ADDR16_LO";
        case RK_ABS32:         return "R_PPC_ADDR32";
        case RK_DATA_PREL32:   return "R_PPC_REL32";
        default:               return NULL;
        }
    case TARGET_SPARC32:
        /* llvm-mc's SPARC .reloc knows these by name */
        switch (kind) {
        case RK_CALL:        return "R_SPARC_WDISP30";
        case RK_SPARC_HI22:  return "R_SPARC_HI22";
        case RK_SPARC_LO10:  return "R_SPARC_LO10";
        case RK_ABS32:       return "R_SPARC_32";
        case RK_DATA_PREL32: return "R_SPARC_DISP32";
        default:             return NULL;
        }
    case TARGET_COLDFIRE:
        switch (kind) {
        case RK_CALL:        return "R_68K_32";      /* jsr to an address */
        case RK_TAIL:        return "R_68K_32";
        case RK_ABS32:       return "R_68K_32";
        case RK_DATA_PREL32: return "R_68K_PC32";
        default:             return NULL;
        }
    case TARGET_AVR:
        /* (without this case AVR fell through to x86-64's names) */
        switch (kind) {
        case RK_CALL: case RK_AVR_CALL: case RK_AVR_TEXT_CALL:
                                 return "R_AVR_CALL";
        case RK_AVR_LO8_LDI:     return "R_AVR_LO8_LDI";
        case RK_AVR_HI8_LDI:     return "R_AVR_HI8_LDI";
        case RK_AVR_LO8_LDI_GS:  return "R_AVR_LO8_LDI_GS";
        case RK_AVR_HI8_LDI_GS:  return "R_AVR_HI8_LDI_GS";
        case RK_AVR_ABS16:       return "R_AVR_16";
        case RK_AVR_ABS16_PM:    return "R_AVR_16_PM";
        case RK_ABS32:           return "R_AVR_32";
        default:                 return NULL;
        }
    case TARGET_X86_64:
    default:
        switch (kind) {
        case RK_CALL:        return "R_X86_64_PLT32";
        case RK_PCREL32:     return "R_X86_64_PC32";
        case RK_ABS64:       return "R_X86_64_64";
        case RK_ABS32:       return "R_X86_64_32";
        case RK_DATA_PREL32: return "R_X86_64_PC32";
        case RK_TPOFF32:     return "R_X86_64_TPOFF32";
        default:             return "R_X86_64_PC32";
        }
    }
}

/* x86-64 counts a PC-relative displacement from the END of the
 * instruction and every other target here counts it from the start, so
 * the -4 that corrects for that is x86's alone -- and only for the kinds
 * that ARE PC-relative: a TPOFF32 is an offset from the thread pointer.
 * The object writer asks target_reloc_addend() the same question, so the
 * text and the object cannot answer it differently. */
static long addend_for(int kind)
{
    return target_reloc_addend(target_get(), (enum reloc_kind)kind, 0);
}

/* MIPS: a load or store's mnemonic, from its major opcode, for a LO16
 * relocation on one; NULL for anything else. */
static const char *mips_ls_name(int op)
{
    switch (op) {
    case 0x20: return "lb";   case 0x21: return "lh";   case 0x22: return "lwl";
    case 0x23: return "lw";   case 0x24: return "lbu";  case 0x25: return "lhu";
    case 0x26: return "lwr";  case 0x28: return "sb";   case 0x29: return "sh";
    case 0x2a: return "swl";  case 0x2b: return "sw";   case 0x2e: return "swr";
    case 0x30: return "ll";   case 0x38: return "sc";
    default:   return NULL;
    }
}

/* A block whose bytes are in a code section of their own (a naked
 * function's section attribute): into it before the block, aligned as the
 * object's section is, and back to .text after. */
static void block_section(struct outbuf *b, const struct topasm *ta, int in)
{
    if (!ta->section)
        return;
    if (in) {
        int al = target_get() == TARGET_AVR ? 2 : 4;
        if (ta->align > al)
            al = ta->align;
        ob_fmt(b, "\t.section\t%s,\"ax\",%sprogbits\n\t.balign\t%d\n",
               asym(ta->section), type_sigil(), al);
    } else {
        ob_str(b, "\t.text\n");
    }
}

/* MIPS: the file-scope blocks and naked functions in .text, from `from`
 * to the end, with their labels and relocations -- written symbolically,
 * as the functions' are (llvm-mc's MIPS .reloc knows none of R_MIPS_26,
 * HI16 or LO16). A field against the block's own assembler-local label
 * is spelt from a label at the block's start. Bytes between blocks are
 * bytes. */
static void mips_emit_blocks(struct outbuf *b, const char *srcname,
                             struct unit *u, const unsigned char *text,
                             long from, long textlen)
{
    long pc = from;
    int nb = 0;
    for (;;) {
        const struct topasm *ta = NULL;
        for (const struct topasm *t = u->topasm; t; t = t->next)
            if (t->codelen > 0 && t->text_off >= pc &&
                (!ta || t->text_off < ta->text_off))
                ta = t;
        long stop = ta ? ta->text_off : textlen;
        if (stop > pc) {
            ob_str(b, "\t.byte\t");
            for (long k = pc; k < stop; k++)
                ob_fmt(b, "%s0x%02x", k > pc ? "," : "", (unsigned)text[k]);
            ob_str(b, "\n");
            pc = stop;
        }
        if (!ta)
            break;
        block_section(b, ta, 1);
        ob_fmt(b, ".Ltopasm%d:\n", nb);
        for (long k = 0; k <= ta->codelen; k++) {
            for (int j = 0; j < ta->nsyms; j++) {
                const struct asmsym *as = &ta->syms[j];
                if (as->off != k)
                    continue;
                if (as->is_global)
                    ob_fmt(b, "\t.%s\t%s\n", as->is_weak ? "weak" : "globl",
                           csym(as->name));
                if (as->type == ASMSYM_FUNC || as->type == ASMSYM_OBJECT)
                    ob_fmt(b, "\t.type\t%s, @%s\n", csym(as->name),
                           as->type == ASMSYM_FUNC ? "function" : "object");
                ob_fmt(b, "%s:\n", csym(as->name));
            }
            if (k == ta->codelen)
                break;
            const struct asmrel *r = NULL;
            for (int j = 0; j < ta->nrels; j++)
                if (ta->rels[j].off == k)
                    r = &ta->rels[j];
            if (r && k + 4 <= ta->codelen) {
                const unsigned char *q = text + ta->text_off + k;
                unsigned long w = (unsigned long)target_get_uint(q, 4);
                int op = (int)(w >> 26), rs = (int)(w >> 21) & 31,
                    rt = (int)(w >> 16) & 31;
                char sym[200];
                if (r->target)
                    snprintf(sym, sizeof sym, "%s%+ld", csym(r->target),
                             r->addend);
                else
                    snprintf(sym, sizeof sym, ".Ltopasm%d%+ld", nb, r->addend);
                if (r->elf_type == R_MIPS_26 && (op == 2 || op == 3))
                    ob_fmt(b, "\t%s\t%s\n", op == 3 ? "jal" : "j", sym);
                else if (r->elf_type == R_MIPS_HI16 && op == 0x0f)
                    ob_fmt(b, "\tlui\t$%d, %%hi(%s)\n", rt, sym);
                else if (r->elf_type == R_MIPS_LO16 && op == 0x09)
                    ob_fmt(b, "\taddiu\t$%d, $%d, %%lo(%s)\n", rt, rs, sym);
                else if (r->elf_type == R_MIPS_LO16 && mips_ls_name(op))
                    ob_fmt(b, "\t%s\t$%d, %%lo(%s)($%d)\n", mips_ls_name(op),
                           rt, sym, rs);
                else if (r->elf_type == R_MIPS_32)
                    ob_fmt(b, "\t.4byte\t%s\n", sym);
                else
                    diag_fatal(srcname, 0, "-S cannot spell a MIPS asm "
                               "block's relocation of type %d on 0x%08lx",
                               r->elf_type, w);
                k += 3;
                continue;
            }
            /* bytes up to the next label or relocation, four a line */
            {
                long e = k + 1;
                while (e < ta->codelen && e - k < 4) {
                    int stop = 0;
                    for (int j = 0; j < ta->nsyms; j++)
                        stop |= ta->syms[j].off == e;
                    for (int j = 0; j < ta->nrels; j++)
                        stop |= ta->rels[j].off == e;
                    if (stop)
                        break;
                    e++;
                }
                ob_str(b, "\t.byte\t");
                for (long x = k; x < e; x++)
                    ob_fmt(b, "%s0x%02x", x > k ? "," : "",
                           (unsigned)text[ta->text_off + x]);
                ob_str(b, "\n");
                k = e - 1;
            }
        }
        for (int j = 0; j < ta->nsyms; j++)
            if (ta->syms[j].size)
                ob_fmt(b, "\t.size\t%s, %ld\n", csym(ta->syms[j].name),
                       ta->syms[j].size);
        block_section(b, ta, 0);
        pc = ta->text_off + ta->codelen;
        nb++;
    }
}

void asm_emit_unit(struct outbuf *b, const char *srcname, struct unit *u,
                   struct ir_unit *iu, const unsigned char *text, long textlen,
                   const unsigned char *rodata,
                   struct extcall *ext, int next,
                   struct strsite *strs, int nstrs,
                   struct gsite *gs, int ngs,
                   struct fsite *fs, int nfs)
{
    /* every relocation site, in .text order */
    int nsite = next + nstrs + ngs + nfs, ns = 0;
    struct site *site = xcalloc((size_t)(nsite ? nsite : 1), sizeof *site);
    for (int i = 0; i < next; i++) {
        site[ns].off = ext[i].patch_off;
        site[ns].name = ext[i].callee ? csym(ext[i].callee->name) : "?";
        site[ns++].kind = ext[i].tail ? RK_TAIL : RK_CALL;
    }
    for (int i = 0; i < nfs; i++) {
        site[ns].off = fs[i].patch_off;
        site[ns].name = fs[i].target ? csym(fs[i].target->name) : "?";
        site[ns].addend = fs[i].addend;
        site[ns++].kind = fs[i].kind;
    }
    for (int i = 0; i < ngs; i++) {
        site[ns].off = gs[i].patch_off;
        site[ns].name = gs[i].glob ? csym(gs[i].glob->name) : "?";
        site[ns++].kind = gs[i].kind;
    }
    for (int i = 0; i < nstrs; i++) {
        site[ns].off = strs[i].patch_off;
        site[ns].name = NULL;
        site[ns].str_off = strs[i].str_off;
        site[ns++].kind = strs[i].kind;
    }
    qsort(site, (size_t)ns, sizeof *site, site_cmp);

    ob_fmt(b, "\t.file\t\"%s\"\n", srcname);
    /* ARMv7-M is Thumb-only, and an assembler defaults to ARM state.
     * Without these the bytes are placed correctly and every symbol is
     * marked ARM, so a caller interworks into the wrong instruction
     * set. `.syntax unified` because the pre-UAL syntax is still the
     * default in some assemblers. */
    if (target_get() == TARGET_THUMB)
        ob_str(b, arm_thumb() ? "\t.syntax unified\n\t.thumb\n"
                              : "\t.syntax unified\n\t.arm\n");
    attrs_emit(b);
    /* MIPS: the code is already scheduled -- its delay slots are filled --
     * and uses $at itself, so the assembler may neither reorder, nor fill
     * a slot, nor expand a macro through $at. */
    if (target_is_mips())
        ob_str(b, "\t.set\tnoreorder\n\t.set\tnoat\n\t.set\tnomacro\n");
    ob_str(b, "\t.text\n");
    /* the section on the strictest start of its functions, where one asks
     * more than the sixteen an assembler gives .text (aligned(N)) */
    {
        int al = 16;
        for (int i = 0; i < iu->nfuncs; i++)
            if (iu->funcs[i].src && !iu->funcs[i].src->section &&
                iu->funcs[i].src->code_align > al)
                al = iu->funcs[i].src->code_align;
        if (al > 16)
            ob_fmt(b, "\t.balign\t%d\n", al);
    }
    long prev_end = 0;
    /* The most recent auipc's local label, for the addi that pairs with
     * it, and a counter so the labels are unique within the unit. */
    char hi_label[32] = "";
    int pcrel_n = 0;            /* where the previous function's code ended */

    /* A function with a section attribute is in that section, after
     * .text's (the driver lays them out last, grouped): the padding before
     * the first one stays in .text, as in the object, and padding between
     * two groups is in neither. */
    const char *cursec = NULL;            /* NULL: .text */
    for (int i = 0; i < iu->nfuncs; i++) {
        struct ir_func *f = &iu->funcs[i];
        if (!f->src || f->src->code_len <= 0)
            continue;
        long lo = f->src->code_off, hi = lo + f->src->code_len;
        const char *sec = f->src->section;
        if (sec != cursec && (!sec || !cursec || strcmp(sec, cursec))) {
            if (!cursec && lo > prev_end) {    /* .text's tail padding */
                ob_str(b, "\t.byte\t");
                for (long k = prev_end; k < lo; k++)
                    ob_fmt(b, "%s0x%02x", k > prev_end ? "," : "",
                           (unsigned)text[k]);
                ob_str(b, "\n");
            }
            prev_end = lo;
            if (sec)
                ob_fmt(b, "\n\t.section\t%s,\"ax\",%sprogbits\n",
                       asym(sec), type_sigil());
            else
                ob_str(b, "\n\t.text\n");
            cursec = sec;
        }
        ob_str(b, "\n");
        /* codegen aligns each function to 16 (code_align in codegen.c), and
         * the padding lies BETWEEN functions, so the instruction loop below
         * never sees it. `.p2align` would let the assembler choose its own
         * nop encoding -- GNU as picks a multi-byte one -- so the actual
         * bytes are emitted instead. Exact by construction, which is the
         * same principle the rest of this file rests on. */
        if (lo > prev_end) {
            ob_str(b, "\t.byte\t");
            for (long k = prev_end; k < lo; k++)
                ob_fmt(b, "%s0x%02x", k > prev_end ? "," : "",
                       (unsigned)text[k]);
            ob_str(b, "\n");
        }
        if (!f->is_static)
            ob_fmt(b, "\t.%s\t%s\n", f->src->is_weak ? "weak" : "globl",
                   csym(f->name));
        if (arm_thumb())
            ob_fmt(b, "\t.thumb_func\n");
        /* A RISC-V interrupt handler is four-aligned with the C
         * extension too (mtvec's low bits are its mode): the padding is
         * already in the bytes above, and this raises the SECTION's
         * alignment, which an assembler otherwise leaves at two. */
        if (f->src->is_isr && (target_get() == TARGET_RISCV32 ||
                               target_get() == TARGET_RISCV64))
            ob_str(b, "\t.p2align\t2\n");
        ob_fmt(b, "\t.type\t%s, %sfunction\n", csym(f->name),
               type_sigil());

        /* The symbol is at the function's ENTRY: Xtensa's literal pool
         * comes first (l32r reaches only backwards), words of data that
         * belong to the function and are not its code. */
        long entry = lo + f->src->code_entry;
        long pc = lo;
        while (pc < hi) {
            char dis[256];
            if (pc == entry)
                ob_fmt(b, "%s:\n", csym(f->name));
            text_labels(b, site, ns, pc);
            /* The target's own length rule first. It is exact on every
             * fixed-width machine and costs no disassembler; only
             * x86-64 returns 0 ("decode it to find out"), and only
             * x86-64 has a disassembler here to do that. Asking the
             * x86-64 decoder on a RISC-V stream -- which is what this
             * did -- mis-groups the bytes AND annotates them with the
             * wrong mnemonics. */
            int have_dis = 0;
            int len = target_insn_len((const unsigned char *)text + pc,
                                      (int)(hi - pc));
#if FAMILY_BUILT(x86)       /* the decoder is x86-64's (make TARGETS=) */
            if (len < 1 && target_get() == TARGET_X86_64) {
                len = embdbg_decode_one(text + pc, (int)(hi - pc),
                                        (unsigned long)pc, dis);
                have_dis = len >= 1;
            }
#endif
            /* RX is one to eight bytes with no rule short of decoding,
             * and there is no RX decoder here: its bytes go out in runs
             * -- data either way -- cut below wherever a relocation, a
             * label or the entry needs a line of its own. */
            if (len < 1)
                len = target_get() == TARGET_X86_64 ? 1 : 8;
            if (pc < entry)
                len = 4;                    /* a pool word */
            if (len > hi - pc)
                len = (int)(hi - pc);
            if (pc < entry && pc + len > entry)
                len = (int)(entry - pc);
            len = cut_group(site, ns, pc, len);
            /* An absolute address in .text -- a jump table's entry -- is
             * data: eight bytes of their own, and what came before it
             * stops where it starts, so no decode of the bytes around a
             * table can swallow one of its relocations. */
            for (int k = 0; k < ns; k++)
                if (site[k].kind == RK_ABS64 && site[k].off >= pc &&
                    site[k].off < pc + len) {
                    len = site[k].off == pc ? 8 : (int)(site[k].off - pc);
                    have_dis = 0;
                    break;
                }
            struct site *st = site_in(site, ns, (int)pc, (int)(pc + len));
            /* RISC-V splits an address across auipc + addi, and the LOW
             * half's relocation names the auipc's ADDRESS rather than
             * the symbol -- that is how the psABI lets the linker find
             * the displacement the high half rounded. An assembler
             * spells it with a local label on the auipc, so that is
             * what is emitted here. The pair is always adjacent (the
             * object writer relies on the same fact: patch_off - 4). */
            if (st && st->kind == RK_RISCV_PCREL_HI20) {
                snprintf(hi_label, sizeof hi_label, ".Lpcrel_hi%d", pcrel_n++);
                ob_fmt(b, "%s:\n", hi_label);
            }
            /* An instruction that names nothing is emitted as its BYTES,
             * with the disassembly as a comment.
             *
             * Not for readability -- for correctness. An assembler chooses
             * among encodings: `sub $0x10,%rsp` it writes in four bytes
             * where the backend wrote seven, and the reassembled program
             * is then a different one. Every such choice moved offsets and
             * broke programs; emitting the bytes removes the choice, and
             * the object is identical by construction.
             *
             * Instructions that DO name something must stay symbolic -- a
             * relocation cannot be spelled in a .byte -- and those forms
             * (a call, a rip-relative lea, an absolute imm32) have one
             * encoding each, so the assembler has nothing to choose. */
            /* MIPS: a relocated instruction is written symbolically --
             * `jal f`, `lui $2, %hi(g)`, `addiu $2, $2, %lo(g)` -- because
             * llvm-mc's MIPS .reloc knows none of R_MIPS_26, HI16 or LO16.
             * Each of these has exactly one encoding, so the assembler has
             * nothing to choose, and a REL assembler stores the addend in
             * the field as EmbCC's object writer does. */
            if (st && target_is_mips() && len == 4) {
                unsigned long w = (unsigned long)target_get_uint(text + pc,
                                                                 4);
                int op = (int)(w >> 26), rs = (int)(w >> 21) & 31,
                    rt = (int)(w >> 16) & 31;
                char sym[200];
                long add = st->addend;
                if (st->kind == RK_MIPS_TEXT26) {
                    snprintf(sym, sizeof sym, "%s", csym(f->name));
                    add = (long)st->str_off - lo;
                } else if (st->name) {
                    snprintf(sym, sizeof sym, "%s", st->name);
                } else {
                    str_label(sym, sizeof sym, iu, st->str_off);
                }
                if ((st->kind == RK_CALL || st->kind == RK_TAIL ||
                     st->kind == RK_MIPS_TEXT26) && (op == 2 || op == 3))
                    ob_fmt(b, "\t%s\t%s%+ld\n", op == 3 ? "jal" : "j",
                           sym, add);
                else if (st->kind == RK_MIPS_HI16 && op == 0x0f)
                    ob_fmt(b, "\tlui\t$%d, %%hi(%s%+ld)\n", rt, sym, add);
                /* MIPS64's four pieces: lui %highest, then daddiu
                 * %higher, %hi and %lo with dsll 16 between */
                else if (st->kind == RK_MIPS_HIGHEST && op == 0x0f)
                    ob_fmt(b, "\tlui\t$%d, %%highest(%s%+ld)\n", rt, sym,
                           add);
                else if ((st->kind == RK_MIPS_HIGHER ||
                          st->kind == RK_MIPS_HI16 ||
                          st->kind == RK_MIPS_LO16) && op == 0x19)
                    ob_fmt(b, "\tdaddiu\t$%d, $%d, %%%s(%s%+ld)\n", rt, rs,
                           st->kind == RK_MIPS_HIGHER ? "higher"
                           : st->kind == RK_MIPS_HI16 ? "hi" : "lo",
                           sym, add);
                else if (st->kind == RK_MIPS_LO16 && op == 0x09)
                    ob_fmt(b, "\taddiu\t$%d, $%d, %%lo(%s%+ld)\n", rt, rs,
                           sym, add);
                else
                    diag_fatal(srcname, 0, "-S cannot spell a MIPS "
                               "relocation of kind %d on instruction "
                               "0x%08lx", st->kind, w);
                st->done = 1;
                pc += len;
                continue;
            }
            ob_str(b, "\t.byte\t");
            for (int k = 0; k < len; k++)
                ob_fmt(b, "%s0x%02x", k ? "," : "",
                       (unsigned)text[pc + k]);
            /* The comment is a convenience and must never be a guess:
             * printed only where a disassembler for THIS target
             * produced it. */
            if (have_dis)
                ob_fmt(b, "\t# %s\n", dis);
            else
                ob_str(b, "\n");
            /* A relocation is attached explicitly rather than spelled into
             * the instruction. Written symbolically, an assembler that can
             * SEE the target resolves it itself -- `lea add(%rip)` becomes a
             * fixed displacement and no relocation at all -- which is a
             * correct program but not the same object. `.reloc` says where
             * and against what, and leaves the bytes alone. */
            if (st) {
                char sym[160];
                if (st->name)
                    snprintf(sym, sizeof sym, "%s", st->name);   /* the site table holds csym() names */
                else if (text_site(st->kind))
                    text_label(sym, sizeof sym, st->str_off);
                else
                    str_label(sym, sizeof sym, iu, st->str_off);
                const char *rt = reloc_name(st->kind);
                if (st->kind == RK_RISCV_PCREL_LO12_I) {
                    if (!*hi_label)
                        diag_fatal(srcname, 0,
                                   "-S: a RISC-V PCREL_LO12 relocation with "
                                   "no PCREL_HI20 before it; the two halves "
                                   "of an address must be emitted as a pair");
                    snprintf(sym, sizeof sym, "%s", hi_label);
                }
                /* Refuse rather than emit a relocation that assembles
                 * into a different program (THE RULE). The one kind
                 * that lands here is RISC-V's paired low half, whose
                 * .reloc operand is the address of the auipc it pairs
                 * with and not the symbol -- emitting the symbol would
                 * assemble cleanly and compute the wrong address. */
                if (!rt)
                    diag_fatal(srcname, 0,
                               "-S cannot yet spell one of this unit's "
                               "relocations for %s (kind %d); the object "
                               "(-c) is correct, and emitting assembly "
                               "that is not the object would be worse "
                               "than refusing",
                               target_triple_now(), st->kind);
                long field = (long)st->off - pc;   /* within the instruction */
                long tail = len - field - (st->kind == RK_ABS64 ? 8 : 4);
                ob_fmt(b, "\t.reloc\t.-%ld, %s, %s%+ld\n",
                       len - field, rt, sym,
                       st->kind == RK_RISCV_PCREL_LO12_I
                           ? 0L : addend_for(st->kind) + st->addend);
                (void)tail;
                st->done = 1;
            }
            pc += len;
        }
        ob_fmt(b, "\t.size\t%s, .-%s\n", csym(f->name), csym(f->name));
        prev_end = hi;
    }
    /* Every relocation the backend recorded is in a function's code, and
     * each must have been written -- as must the label each text site
     * names. One lost here assembles cleanly into a different program. */
    for (int k = 0; k < ns; k++)
        if (!site[k].done || (text_site(site[k].kind) && !site[k].labeled))
            diag_fatal(srcname, 0, "-S lost a relocation at .text+%d "
                       "(kind %d%s); the object (-c) is correct, and "
                       "emitting assembly that is not the object would be "
                       "worse than refusing", site[k].off, site[k].kind,
                       site[k].done ? ", its target starts no instruction"
                                    : "");
    /* The asm blocks and naked functions, in .text after the functions:
     * their labels and relocations, where the targets below have them. */
    if (blocks_by_reloc() && u->topasm) {
        if (cursec) {
            ob_str(b, "\n\t.text\n");
            cursec = NULL;
        }
        blocks_emit(b, u, text, prev_end, textlen);
        prev_end = textlen;
    }
    /* .text may end with padding too, and the file-scope asm after it --
     * in .text, whatever section the last function was in. */
    if (cursec)
        ob_str(b, "\n\t.text\n");
    if (target_is_mips() && u->topasm) {
        mips_emit_blocks(b, srcname, u, text, prev_end, textlen);
        prev_end = textlen;
    }
    if (textlen > prev_end) {
        ob_str(b, "\t.byte\t");
        for (long k = prev_end; k < textlen; k++)
            ob_fmt(b, "%s0x%02x", k > prev_end ? "," : "",
                   (unsigned)text[k]);
        ob_str(b, "\n");
    }
    /* An alias is its target's address under another name; on Thumb
     * .thumb_set, so the symbol keeps the bit that says Thumb code. */
    for (struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed || !f->alias_of)
            continue;
        if (!f->is_static)
            ob_fmt(b, "\t.%s\t%s\n", f->is_weak ? "weak" : "globl",
                   csym(f->name));
        ob_fmt(b, "\t.type\t%s, %sfunction\n", csym(f->name), type_sigil());
        ob_fmt(b, "\t.%s\t%s, %s\n",
               arm_thumb() ? "thumb_set" : "set",
               csym(f->name), csym(f->alias_of));
        /* its target's size too, as the object gives it (main.c) */
        for (struct func *t = u->funcs; t; t = t->next)
            if (!t->absorbed && t->has_defn && !strcmp(t->name, f->alias_of)) {
                ob_fmt(b, "\t.size\t%s, %d\n", csym(f->name),
                       t->code_len - t->code_entry);
                break;
            }
    }
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->alias_of)
            continue;
        if (!g->is_static)
            ob_fmt(b, "\t.%s\t%s\n", g->is_weak ? "weak" : "globl",
                   csym(g->name));
        ob_fmt(b, "\t.type\t%s, %sobject\n", csym(g->name), type_sigil());
        ob_fmt(b, "\t.set\t%s, %s\n", csym(g->name), csym(g->alias_of));
        for (struct global *t = u->globals; t; t = t->next)
            if (!t->absorbed && t->defined && !strcmp(t->name, g->alias_of)) {
                ob_fmt(b, "\t.size\t%s, %d\n", csym(g->name),
                       global_size(t));
                break;
            }
    }

    /* .rodata: the string literals, each under the label the code refers
     * to. Emitted as bytes, because a string may hold anything. */
    if (rodata && iu->rodata_len > 0) {
        ob_str(b, "\n\t.section\t.rodata\n");
        /* the object's .rodata is 16-aligned (main.c); an assembler's
         * starts at 1 unless told */
        ob_str(b, "\t.balign\t16\n");
        long cur = 0;
        for (int i = 0; i < iu->nstrs; i++) {
            /* the same gap irgen left, as zeros, so each string is at
             * the offset the code's relocations were computed for */
            if (iu->strs[i].off < cur)
                internal_error("-S: string %d at .rodata+%d overlaps the "
                               "one before it", i, iu->strs[i].off);
            if (iu->strs[i].off > cur)
                ob_fmt(b, "\t.zero\t%ld\n", iu->strs[i].off - cur);
            if (iu->strs[i].align > 1)
                ob_fmt(b, "\t.balign\t%d\n", iu->strs[i].align);
            cur = iu->strs[i].off + iu->strs[i].len;
            ob_fmt(b, ".LC%d:\n", i);
            ob_str(b, "\t.byte\t");
            for (int k = 0; k < iu->strs[i].len; k++)
                ob_fmt(b, "%s%d", k ? "," : "",
                       (int)(unsigned char)iu->strs[i].bytes[k]);
            ob_str(b, "\n");
        }
    }

    /* Globals defined here, where the object put them. main.c laid out
     * every data section before this runs -- g->off, in_bss, in_rodata,
     * named, is_tls -- and the text follows that layout, each object at
     * its offset with the gap before it written as zeros, rather than
     * deriving one of its own. The one it derived was the -S of another
     * object: _Alignas(64) and a section attribute dropped, an empty
     * struct left undefined, a TLS object put in .data. */
    long rodata_end = 0;
    for (int i = 0; i < iu->nstrs; i++)
        if (iu->strs[i].off + iu->strs[i].len > rodata_end)
            rodata_end = iu->strs[i].off + iu->strs[i].len;
    globals_emit(b, u, iu, GS_DATA, 0, NULL, 0);
    globals_emit(b, u, iu, GS_BSS, 0, NULL, 0);
    globals_emit(b, u, iu, GS_RODATA, 0, NULL,
                 rodata && iu->rodata_len > 0 ? rodata_end : -1);
    globals_emit(b, u, iu, GS_TDATA, 0, NULL, 0);
    globals_emit(b, u, iu, GS_TBSS, 0, NULL, 0);
    for (int k = 1; ; k++) {
        const struct global *first = NULL;
        for (struct global *g = u->globals; g && !first; g = g->next)
            if (!g->absorbed && g->defined && !g->is_common && g->named == k)
                first = g;
        if (!first)
            break;
        globals_emit(b, u, iu, GS_NAMED, k, first->section, 0);
    }
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->defined && g->is_common) {
            /* -fcommon: the linker places it, at the alignment main.c
             * left in its offset (SHN_COMMON's st_value) */
            ob_fmt(b, "\t.comm\t%s,%d,%d\n", csym(g->name), global_size(g),
                   g->off);
        }

    /* What the object binds besides its definitions: a WEAK reference to
     * a symbol defined elsewhere -- undefined, and zero when nothing
     * defines it, as an RTOS's optional hook is -- and each visibility
     * asked for. An assembler makes a symbol of a .weak name even when
     * nothing refers to it, so only the referenced ones are named, as
     * the object has only those (main.c). */
    for (struct func *f = u->funcs; f; f = f->next) {
        if (f->absorbed || f->has_defn || f->alias_of || !f->is_weak ||
            topasm_defines(u, f->name))
            continue;
        int refd = 0;
        for (int i = 0; i < next && !refd; i++)
            refd = ext[i].callee && !strcmp(ext[i].callee->name, f->name);
        for (int i = 0; i < nfs && !refd; i++)
            refd = fs[i].target && !strcmp(fs[i].target->name, f->name);
        for (struct global *g = u->globals; g && !refd; g = g->next)
            for (int q = 0; !g->absorbed && q < g->nrelocs && !refd; q++)
                refd = g->relocs[q].ftarget &&
                       !strcmp(g->relocs[q].ftarget->name, f->name);
        if (refd)
            ob_fmt(b, "\t.weak\t%s\n", csym(f->name));
    }
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && !g->defined && !g->alias_of && g->is_weak &&
            g->used && !topasm_defines(u, g->name))
            ob_fmt(b, "\t.weak\t%s\n", csym(g->name));
    for (struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && f->vis && strcmp(f->vis, "default"))
            ob_fmt(b, "\t.%s\t%s\n", f->vis, csym(f->name));
    for (struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && g->vis && strcmp(g->vis, "default"))
            ob_fmt(b, "\t.%s\t%s\n", g->vis, csym(g->name));

    /* __attribute__((constructor)) / ((destructor)). The object writer
     * puts these in SHT_INIT_ARRAY / SHT_FINI_ARRAY sections, so `-S`
     * has to as well: assembly that assembles into a DIFFERENT program
     * from the one `-c` emits is the one thing this output may not be.
     * A constructor missing here would not fail to build -- it would
     * simply never run, which is the failure the attribute was
     * implemented to end.
     *
     * "aw" and @init_array give the section the same flags and TYPE the
     * writer uses; a linker gathers these by type, so @progbits here
     * would lay the pointers out as ordinary data. */
    /* A priority's own section, `.init_array.NNNNN`, as the object writer
     * makes it (constructor(N)): one section per priority, in the order
     * the functions first ask for it, each holding its functions in
     * source order. */
    static const char *const arr[2] = { ".init_array", ".fini_array" };
    for (int pass = 0; pass < 2; pass++)
        for (struct func *g = u->funcs; g; g = g->next) {
            if (g->absorbed || !g->has_defn ||
                !(pass == 0 ? g->is_ctor : g->is_dtor))
                continue;
            int prio = pass == 0 ? g->ctor_prio : g->dtor_prio, seen = 0;
            /* a priority an earlier function asked for is done */
            for (struct func *e = u->funcs; e != g; e = e->next)
                if (!e->absorbed && e->has_defn &&
                    (pass == 0 ? e->is_ctor : e->is_dtor) &&
                    (pass == 0 ? e->ctor_prio : e->dtor_prio) == prio)
                    seen = 1;
            if (seen)
                continue;
            char nm[32];
            if (prio)
                snprintf(nm, sizeof nm, "%s.%05d", arr[pass], prio - 1);
            else
                snprintf(nm, sizeof nm, "%s", arr[pass]);
            ob_fmt(b, "\n\t.section\t%s,\"aw\",%s%s\n\t.balign\t%d\n",
                   nm, type_sigil(), arr[pass] + 1, target_ptr_size());
            for (struct func *f = g; f; f = f->next)
                if (!f->absorbed && f->has_defn &&
                    (pass == 0 ? f->is_ctor : f->is_dtor) &&
                    (pass == 0 ? f->ctor_prio : f->dtor_prio) == prio)
                    ptr_slot(b, csym(f->name), 0, 1);
        }
}

/* ---- asm blocks and naked functions: Thumb, RISC-V and AVR ------------
 *
 * There a file-scope __asm__ block and a naked function's body are
 * assembled by src/as/gas.c into bytes, labels and relocations (struct
 * topasm), and the object carries all three. -S writes them as it writes
 * a function: the bytes, with every relocated field attached by .reloc.
 * They used to be one run of .byte, which reassembled into an object
 * with none of the block's symbols and none of its relocations.
 *
 * The labels are written as the object has them. A .globl/.weak label is
 * itself, typed as the object types it; so is a local one that names a
 * function or an object C declares and does not define -- a static
 * naked function, chiefly -- since the object gives that one a local
 * symbol. Any other label is not in the object's symbol table at all, so
 * it is written as a .L label of its block's own, `.Lasm<N>.<name>`,
 * which keeps it out of the reassembled symbol table too and cannot
 * clash with a label of another block or of C. A field the object
 * relocates against a place in its own block (src/as/gas.c hands those
 * over as offsets) names the nearest of the block's labels before the
 * place -- its start, `.Lasm<N>`, at least -- plus the distance; or,
 * where it must name a label AT the place (bexact: an ARM .reloc, and
 * the auipc a RISC-V low half names), a label made there,
 * `.Lasm<N>_<offset>`.
 *
 * The first of those is ARM's. An ARM assembler writes REL relocations,
 * whose addend lives in the relocated field, and .reloc leaves the field
 * alone: `.reloc ..., sym+8` silently becomes sym+0 there. So on Thumb
 * no .reloc carries an addend: a data word is written `.long sym+N`,
 * which every assembler relocates whole, a movw/movt of a symbol plus an
 * offset is written as the instruction, and anything else with an addend
 * is refused by name. RISC-V and AVR are RELA, and .reloc's addend is
 * the relocation's. */

/* A label -S writes in a block. */
struct blabel {
    int blk;                    /* its block, numbered in u->topasm order */
    long off;                   /* within the block */
    const char *name;           /* as written */
    const struct asmsym *as;    /* the block's own label, or NULL: a place
                                 * a field is relocated against */
    int sym;                    /* a symbol of the object: global, or local
                                 * and declared by C */
    int ctype;                  /* ASMSYM_*: the symbol's type there */
};

/* What one relocated field of a block is written against. */
struct bref {
    const char *sym;
    long addend;
};

struct bstate {
    const struct topasm **blk;
    int nblk;
    struct blabel *lab;
    int nlab, caplab;
};

/* Every target whose blocks src/as/gas.c assembled -- all but x86-64's
 * (its own assembler) and MIPS's (written symbolically, above). It was
 * five of them, and on the rest a block's relocations were left out of
 * the text without a word: the RX setjmp a block called came out as a
 * call to address zero. */
static int blocks_by_reloc(void)
{
    return !target_is_mips() && target_get() != TARGET_X86_64;
}

static struct blabel *blabel_add(struct bstate *s, int blk, long off)
{
    if (s->nlab == s->caplab) {
        s->caplab = s->caplab ? 2 * s->caplab : 16;
        s->lab = xrealloc(s->lab, (size_t)s->caplab * sizeof *s->lab);
    }
    struct blabel *l = &s->lab[s->nlab++];
    memset(l, 0, sizeof *l);
    l->blk = blk;
    l->off = off;
    return l;
}

static const char *bname(const char *fmt, int blk, const char *name, long off)
{
    char buf[300];
    if (name)
        snprintf(buf, sizeof buf, fmt, blk, name);
    else
        snprintf(buf, sizeof buf, fmt, blk, off);
    return asym(xstrndup(buf, strlen(buf)));
}

/* How the object types a block's LOCAL label: ASMSYM_FUNC or
 * ASMSYM_OBJECT when it is the one that gives a function or an object C
 * declared and did not define its symbol, else 0 (src/driver/main.c's
 * local labels, which take the first such label in block order). */
static int blabel_claim(const struct unit *u, const struct bstate *s,
                        const char *name)
{
    int ct = 0;
    for (const struct func *f = u->funcs; f && !ct; f = f->next)
        if (!f->absorbed && !f->has_defn && !f->alias_of &&
            strcmp(f->name, name) == 0)
            ct = ASMSYM_FUNC;
    for (const struct global *g = u->globals; g && !ct; g = g->next)
        if (!g->absorbed && !g->defined && strcmp(g->name, name) == 0)
            ct = ASMSYM_OBJECT;
    for (int i = 0; i < s->nlab && ct; i++)
        if (s->lab[i].sym && s->lab[i].as && !s->lab[i].as->is_global &&
            strcmp(s->lab[i].as->name, name) == 0)
            ct = 0;                         /* an earlier block's has it */
    return ct;
}

/* Whether a field must name a label AT its target, with nothing added:
 * an ARM .reloc, whose addend an ARM assembler loses (see above), and a
 * RISC-V low half, which names its auipc. */
static int bexact(int type)
{
    switch (target_get()) {
    case TARGET_THUMB:
        return type != R_ARM_ABS32;
    case TARGET_RISCV32:
    case TARGET_RISCV64:
        return type == R_RISCV_PCREL_LO12_I || type == R_RISCV_PCREL_LO12_S;
    default:
        return 0;
    }
}

/* A place in a block, as a label plus an addend: the block's own .L-named
 * label there, or one made for the place -- but not inside a Thumb word
 * that is written as a statement (bspelled), where no label can go, nor
 * outside the block. Those are the block's start plus the distance. */
static void bplace(struct bstate *s, int blk, long x, long bit, int exact,
                   struct bref *r)
{
    const struct topasm *ta = s->blk[blk];
    int room = x >= 0 && x <= ta->codelen;
    for (int j = 0; j < ta->nrels && room; j++) {
        int t = ta->rels[j].elf_type;
        if (target_get() == TARGET_THUMB && x > ta->rels[j].off &&
            x < ta->rels[j].off + 4 &&
            (t == R_ARM_ABS32 || t == R_ARM_THM_MOVW_ABS_NC ||
             t == R_ARM_THM_MOVT_ABS || t == R_ARM_MOVW_ABS_NC ||
             t == R_ARM_MOVT_ABS))
            room = 0;
    }
    if (room) {
        for (int i = 0; i < s->nlab; i++)
            if (s->lab[i].blk == blk && s->lab[i].off == x && !s->lab[i].sym) {
                r->sym = s->lab[i].name;
                r->addend = bit;
                return;
            }
        if (exact) {
            struct blabel *l = blabel_add(s, blk, x);
            l->name = bname(".Lasm%d_%ld", blk, NULL, x);
            r->sym = l->name;
            r->addend = bit;
            return;
        }
    }
    /* the nearest of the block's own labels before it, for the reader */
    const struct blabel *near = NULL;
    for (int i = 0; i < s->nlab && x >= 0 && x <= ta->codelen; i++)
        if (s->lab[i].blk == blk && !s->lab[i].sym && s->lab[i].off <= x &&
            (!near || s->lab[i].off > near->off))
            near = &s->lab[i];
    r->sym = near ? near->name : bname(".Lasm%d", blk, NULL, 0);
    r->addend = x - (near ? near->off : 0) + bit;
}

/* What a block's field names, resolved as src/driver/main.c resolves it
 * for the object: a field against the block's own assembler-local label
 * is its block's start plus the addend; a name is a block's global label,
 * else a function or object of C's, else the first block's local label
 * of that name (a Thumb function's address carrying its bit), else an
 * undefined symbol. */
static void bresolve(const struct unit *u, struct bstate *s, int blk,
                     const struct asmrel *r, struct bref *out)
{
    int exact = bexact(r->elf_type);
    if (!r->target) {
        bplace(s, blk, r->addend, 0, exact, out);
        return;
    }
    out->sym = csym(r->target);
    out->addend = r->addend;
    for (int i = 0; i < s->nlab; i++)
        if (s->lab[i].as && s->lab[i].as->is_global &&
            strcmp(s->lab[i].as->name, r->target) == 0)
            return;
    for (const struct func *f = u->funcs; f; f = f->next)
        if (!f->absorbed && strcmp(f->name, r->target) == 0)
            return;
    for (const struct global *g = u->globals; g; g = g->next)
        if (!g->absorbed && strcmp(g->name, r->target) == 0)
            return;
    for (int i = 0; i < s->nlab; i++) {
        const struct asmsym *as = s->lab[i].as;
        if (!as || strcmp(as->name, r->target) != 0)
            continue;
        long x = as->off + r->addend;
        /* the object ORs the bit in, so an odd place keeps its value */
        long bit = arm_thumb() && as->type == ASMSYM_FUNC &&
                   (!r->elf_type || r->elf_type == R_ARM_ABS32) &&
                   !((s->blk[s->lab[i].blk]->text_off + x) & 1);
        if (!r->addend || !exact) {        /* the label, plus the addend */
            out->sym = s->lab[i].name;
            out->addend = r->addend + bit;
            return;
        }
        bplace(s, s->lab[i].blk, x, bit, 1, out);
        return;
    }
}

/* Thumb: a field written as its own statement rather than as bytes and
 * a .reloc -- a data word, and a movw/movt whose addend is not 0. */
static int bspelled(const struct asmrel *r, const struct bref *ref)
{
    if (target_get() != TARGET_THUMB)
        return 0;
    return r->elf_type == R_ARM_ABS32 ||
           ((r->elf_type == R_ARM_THM_MOVW_ABS_NC ||
             r->elf_type == R_ARM_THM_MOVT_ABS) && ref->addend);
}

/* A data word's width, by the relocation on it; 0 for an instruction's. */
static long bdata_width(int type)
{
    switch (target_get()) {
    case TARGET_THUMB:
        return type == R_ARM_ABS32 ? 4 : 0;
    case TARGET_RISCV32:
    case TARGET_RISCV64:
        return type == R_RISCV_32 ? 4 : type == R_RISCV_64 ? 8 : 0;
    case TARGET_AVR:
        return type == R_AVR_16 || type == R_AVR_16_PM ? 2
             : type == R_AVR_32 ? 4 : 0;
    case TARGET_LOONGARCH64:
        return type == R_LARCH_32 ? 4 : type == R_LARCH_64 ? 8 : 0;
    default:
        return 0;
    }
}

static void bsym_ref(struct outbuf *b, const struct bref *ref)
{
    if (ref->addend)
        ob_fmt(b, "%s%+ld", ref->sym, ref->addend);
    else
        ob_str(b, ref->sym);
}

static void bytes_line(struct outbuf *b, const unsigned char *p, long n)
{
    ob_str(b, "\t.byte\t");
    for (long k = 0; k < n; k++)
        ob_fmt(b, "%s0x%02x", k ? "," : "", (unsigned)p[k]);
    ob_str(b, "\n");
}

/* The file-scope blocks and naked functions, which follow the functions
 * in .text, from `from` to the end. */
static void blocks_emit(struct outbuf *b, struct unit *u,
                        const unsigned char *text, long from, long textlen)
{
    struct bstate s;
    int thumb = arm_thumb();        /* (ARMv7-A's A32 state has no bit) */
    memset(&s, 0, sizeof s);
    for (const struct topasm *t = u->topasm; t; t = t->next)
        s.nblk++;
    s.blk = xcalloc((size_t)(s.nblk ? s.nblk : 1), sizeof *s.blk);
    {
        int i = 0;
        for (const struct topasm *t = u->topasm; t; t = t->next)
            s.blk[i++] = t;
    }
    /* the blocks' own labels, first all of them, for the references */
    for (int i = 0; i < s.nblk; i++)
        for (int j = 0; j < s.blk[i]->nsyms; j++) {
            const struct asmsym *as = &s.blk[i]->syms[j];
            int ct = as->is_global ? 0 : blabel_claim(u, &s, as->name);
            struct blabel *l = blabel_add(&s, i, as->off);
            l->as = as;
            if (as->is_global) {
                /* untyped, the object makes it a function -- but on
                 * Thumb, where a function's address carries a bit */
                l->sym = 1;
                l->ctype = as->type != ASMSYM_UNTYPED ? as->type
                         : thumb ? ASMSYM_UNTYPED : ASMSYM_FUNC;
                l->name = csym(as->name);
            } else if (ct) {
                l->sym = 1;
                l->ctype = ct;
                l->name = csym(as->name);
            } else {
                l->name = bname(".Lasm%d.%s", i, as->name, 0);
            }
        }
    /* what each relocated field names, which adds the places */
    struct bref **ref = xcalloc((size_t)(s.nblk ? s.nblk : 1), sizeof *ref);
    for (int i = 0; i < s.nblk; i++) {
        const struct topasm *ta = s.blk[i];
        ref[i] = xcalloc((size_t)(ta->nrels ? ta->nrels : 1), sizeof *ref[i]);
        for (int j = 0; j < ta->nrels; j++)
            bresolve(u, &s, i, &ta->rels[j], &ref[i][j]);
    }

    long pc = from;
    for (int i = 0; i < s.nblk; i++) {
        const struct topasm *ta = s.blk[i];
        const unsigned char *code = text + ta->text_off;
        ob_str(b, "\n");
        if (ta->text_off > pc)                 /* the padding before it */
            bytes_line(b, text + pc, ta->text_off - pc);
        block_section(b, ta, 1);
        ob_fmt(b, "%s:\n", bname(".Lasm%d", i, NULL, 0));
        for (long k = 0; k <= ta->codelen; ) {
            /* the labels here, a block's own first */
            for (int pass = 0; pass < 2; pass++)
                for (int j = 0; j < s.nlab; j++) {
                    const struct blabel *l = &s.lab[j];
                    if (l->blk != i || l->off != k || (pass == 0) != !!l->as)
                        continue;
                    if (l->sym && l->as->is_global)
                        ob_fmt(b, "\t.%s\t%s\n",
                               l->as->is_weak ? "weak" : "globl", l->name);
                    if (l->sym && l->ctype == ASMSYM_FUNC) {
                        if (thumb)
                            ob_str(b, "\t.thumb_func\n");
                        ob_fmt(b, "\t.type\t%s, %sfunction\n", l->name,
                               type_sigil());
                    } else if (l->sym && l->ctype == ASMSYM_OBJECT) {
                        ob_fmt(b, "\t.type\t%s, %sobject\n", l->name,
                               type_sigil());
                    }
                    ob_fmt(b, "%s:\n", l->name);
                }
            if (k == ta->codelen)
                break;
            /* a field written as a statement of its own */
            int done = 0;
            for (int j = 0; j < ta->nrels && !done; j++) {
                const struct asmrel *r = &ta->rels[j];
                if (r->off != k || !bspelled(r, &ref[i][j]))
                    continue;
                for (int q = 0; q < s.nlab; q++)
                    if (s.lab[q].blk == i && s.lab[q].off > k &&
                        s.lab[q].off < k + 4)
                        diag_fatal(ta->file, ta->line, "-S cannot write the "
                                   "asm block's label %s inside a relocated "
                                   "word", s.lab[q].name);
                if (k + 4 > ta->codelen)
                    diag_fatal(ta->file, ta->line, "-S cannot write the asm "
                               "block's relocated word at its end");
                if (r->elf_type == R_ARM_ABS32) {
                    ob_str(b, "\t.long\t");
                } else {
                    unsigned h1 = (unsigned)code[k] | (unsigned)code[k + 1] << 8;
                    unsigned h2 = (unsigned)code[k + 2] |
                                  (unsigned)code[k + 3] << 8;
                    int top = r->elf_type == R_ARM_THM_MOVT_ABS;
                    if ((h1 & 0xfbf0u) != (top ? 0xf2c0u : 0xf240u))
                        diag_fatal(ta->file, ta->line, "-S cannot write the "
                                   "asm block's %s relocation on %04x %04x, "
                                   "which is not a %s", top ? "movt" : "movw",
                                   h1, h2, top ? "movt" : "movw");
                    ob_fmt(b, "\t%s\tr%u, #:%s16:", top ? "movt" : "movw",
                           (h2 >> 8) & 15u, top ? "upper" : "lower");
                }
                bsym_ref(b, &ref[i][j]);
                ob_str(b, "\n");
                k += 4;
                done = 1;
            }
            if (done)
                continue;
            /* an instruction, or a data word -- up to the next label or
             * spelled field, whichever is first */
            long len = 0;
            for (int j = 0; j < ta->nrels && !len; j++)
                if (ta->rels[j].off == k)
                    len = bdata_width(ta->rels[j].elf_type);
            for (int d = 0; d + 1 < ta->ndrange && !len; d += 2)
                if (k >= ta->drange[d] && k < ta->drange[d + 1])
                    len = ta->drange[d + 1] - k < 4 ? ta->drange[d + 1] - k : 4;
            if (!len)
                len = target_insn_len(code + k, (int)(ta->codelen - k));
            if (len < 1)
                len = 1;
            long e = k + len > ta->codelen ? ta->codelen : k + len;
            for (int j = 0; j < s.nlab; j++)
                if (s.lab[j].blk == i && s.lab[j].off > k && s.lab[j].off < e)
                    e = s.lab[j].off;
            for (int j = 0; j < ta->nrels; j++)
                if (ta->rels[j].off > k && ta->rels[j].off < e &&
                    bspelled(&ta->rels[j], &ref[i][j]))
                    e = ta->rels[j].off;
            bytes_line(b, code + k, e - k);
            for (int j = 0; j < ta->nrels; j++) {
                const struct asmrel *r = &ta->rels[j];
                const char *rt;
                if (r->off < k || r->off >= e)
                    continue;
                rt = target_reloc_name(target_get(), r->elf_type);
                if (!rt)
                    diag_fatal(ta->file, ta->line, "-S cannot write the asm "
                               "block's relocation of ELF type %d for %s",
                               r->elf_type, target_triple_now());
                /* An ARM .reloc keeps no addend (REL), and a RISC-V low
                 * half names its auipc; see above. */
                if (bexact(r->elf_type) && ref[i][j].addend)
                    diag_fatal(ta->file, ta->line, "-S cannot write the asm "
                               "block's %s against %s%+ld: %s", rt,
                               ref[i][j].sym, ref[i][j].addend,
                               thumb ? "an ARM assembler takes the addend "
                                       "from the instruction, which .reloc "
                                       "leaves as it is"
                                     : "it must name the auipc it pairs "
                                       "with");
                ob_fmt(b, "\t.reloc\t.-%ld, %s, ", e - r->off, rt);
                bsym_ref(b, &ref[i][j]);
                ob_str(b, "\n");
            }
            k = e;
        }
        for (int j = 0; j < s.nlab; j++)
            if (s.lab[j].blk == i && s.lab[j].sym && s.lab[j].as->size)
                ob_fmt(b, "\t.size\t%s, %ld\n", s.lab[j].name,
                       s.lab[j].as->size);
        block_section(b, ta, 0);
        pc = ta->text_off + ta->codelen;
    }
    if (textlen > pc)
        bytes_line(b, text + pc, textlen - pc);
    for (int i = 0; i < s.nblk; i++)
        free(ref[i]);
    free(ref);
    free(s.lab);
    free(s.blk);
}
