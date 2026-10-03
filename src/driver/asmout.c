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
 * x86-64 only. The aarch64 backend has no disassembler in this repository,
 * so `-S` for that target refuses rather than emitting something that looks
 * like assembly and is not (docs/internals/contributing.md's rule:
 * unsupported must fail, never be silently wrong).
 */
#include "asmout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#include "../arch/backend.h"
#include "../arch/target.h"
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

/* One place in .text that names a symbol. Collected from what the backend
 * recorded, so this is the same information the ELF writer turns into
 * relocations — not a second derivation of it. */
struct site {
    long addend;             /* an RK_ABS64 site's offset into its target */
    int off;                 /* the relocated field's offset in .text */
    const char *name;        /* the symbol, or NULL for a .rodata string */
    int str_off;             /* when name is NULL: offset inside .rodata */
    int kind;                /* enum reloc_kind */
};

static int site_cmp(const void *a, const void *b)
{
    int x = ((const struct site *)a)->off, y = ((const struct site *)b)->off;
    return x < y ? -1 : x > y;
}

/* The site whose field falls inside [lo, hi), or NULL. */
static const struct site *site_in(const struct site *s, int n, int lo, int hi)
{
    for (int i = 0; i < n; i++)
        if (s[i].off >= lo && s[i].off < hi)
            return &s[i];
    return NULL;
}

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
 * the -4 that corrects for that is x86's alone. */
static long addend_for(int kind)
{
    if (kind == RK_ABS64 || kind == RK_ABS32)
        return 0;
    return target_get() == TARGET_X86_64 ? -4 : 0;
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
        site[ns].name = ext[i].callee ? asym(ext[i].callee->name) : "?";
        site[ns++].kind = ext[i].tail ? RK_TAIL : RK_CALL;
    }
    for (int i = 0; i < nfs; i++) {
        site[ns].off = fs[i].patch_off;
        site[ns].name = fs[i].target ? asym(fs[i].target->name) : "?";
        site[ns].addend = fs[i].kind == RK_ABS64 ? fs[i].addend : 0;
        site[ns++].kind = fs[i].kind;
    }
    for (int i = 0; i < ngs; i++) {
        site[ns].off = gs[i].patch_off;
        site[ns].name = gs[i].glob ? asym(gs[i].glob->name) : "?";
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
        ob_str(b, "\t.syntax unified\n\t.thumb\n");
    ob_str(b, "\t.text\n");
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
                   asym(f->name));
        if (target_get() == TARGET_THUMB)
            ob_fmt(b, "\t.thumb_func\n");
        ob_fmt(b, "\t.type\t%s, %sfunction\n%s:\n", asym(f->name),
               type_sigil(), asym(f->name));

        long pc = lo;
        while (pc < hi) {
            char dis[256];
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
            if (len < 1) {
                len = embdbg_decode_one(text + pc, (int)(hi - pc),
                                        (unsigned long)pc, dis);
                have_dis = target_get() == TARGET_X86_64 && len >= 1;
            }
            if (len < 1)
                len = 1;
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
            const struct site *st = site_in(site, ns, (int)pc,
                                            (int)(pc + len));
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
                    snprintf(sym, sizeof sym, "%s", st->name);   /* the site table holds asym() names */
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
            }
            pc += len;
        }
        ob_fmt(b, "\t.size\t%s, .-%s\n", asym(f->name), asym(f->name));
        prev_end = hi;
    }
    /* .text may end with padding too, and the file-scope asm after it --
     * in .text, whatever section the last function was in. */
    if (cursec)
        ob_str(b, "\n\t.text\n");
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
                   asym(f->name));
        ob_fmt(b, "\t.type\t%s, %sfunction\n", asym(f->name), type_sigil());
        ob_fmt(b, "\t.%s\t%s, %s\n",
               target_get() == TARGET_THUMB ? "thumb_set" : "set",
               asym(f->name), asym(f->alias_of));
    }

    /* .rodata: the string literals, each under the label the code refers
     * to. Emitted as bytes, because a string may hold anything. */
    if (rodata && iu->rodata_len > 0) {
        ob_str(b, "\n\t.section\t.rodata\n");
        for (int i = 0; i < iu->nstrs; i++) {
            if (iu->strs[i].align > 1)       /* the same gap irgen left */
                ob_fmt(b, "\t.balign\t%d\n", iu->strs[i].align);
            ob_fmt(b, ".LC%d:\n", i);
            ob_str(b, "\t.byte\t");
            for (int k = 0; k < iu->strs[i].len; k++)
                ob_fmt(b, "%s%d", k ? "," : "",
                       (int)(unsigned char)iu->strs[i].bytes[k]);
            ob_str(b, "\n");
        }
    }

    /* Globals defined here. An initialized one is bytes in .data; a zero
     * one is .bss, where `.comm`-style reservation says the size without
     * writing it. */
    int any_data = 0;
    for (struct global *g = u->globals; g; g = g->next) {
        if (g->absorbed || !g->defined || g->is_extern)
            continue;
        int sz = global_size(g);
        int al = g->ty ? ty_align(g->ty) : 1;
        if (!sz)
            continue;
        if (!any_data) { ob_str(b, "\n"); any_data = 1; }
        if (!g->is_static)
            ob_fmt(b, "\t.%s\t%s\n", g->is_weak ? "weak" : "globl",
                   asym(g->name));
        /* a const object is read-only data, as in the object (main.c) */
        const char *sec = g->in_rodata ? "\t.section\t.rodata\n" : "\t.data\n";
        if (g->init_bytes && g->init_len > 0) {
            ob_str(b, sec);
            /* .balign: a BYTE count everywhere. `.align N` is N bytes on
             * x86 and 2^N on ARM, aarch64 and RISC-V, so an 8-aligned
             * object reassembled 256-aligned there. */
            ob_fmt(b, "\t.balign\t%d\n\t.type\t%s, %sobject\n%s:\n",
                   al, asym(g->name), type_sigil(), asym(g->name));
            /* A pointer slot in an initializer is an ADDRESS the linker
             * fills in, not bytes: `static char *p = "hi";` holds a
             * relocation, and emitting its zeroed bytes would produce a
             * null pointer that links and then crashes. The byte runs
             * between relocations are emitted as bytes; each relocated
             * slot becomes a pointer-wide `symbol + addend` (ptr_slot). */
            int k = 0;
            while (k < sz) {
                const struct greloc *r = NULL;
                for (int q = 0; q < g->nrelocs; q++)
                    if (g->relocs[q].off == k) { r = &g->relocs[q]; break; }
                if (r) {
                    char sym[160];
                    if (r->str)
                        str_label(sym, sizeof sym, iu, r->str_off);
                    else if (r->gtarget)
                        snprintf(sym, sizeof sym, "%s", asym(r->gtarget->name));
                    else if (r->ftarget)
                        snprintf(sym, sizeof sym, "%s", asym(r->ftarget->name));
                    else
                        snprintf(sym, sizeof sym, "0");
                    ptr_slot(b, sym, r->addend, r->ftarget != NULL);
                    k += target_ptr_size();
                    continue;
                }
                /* bytes up to the next relocation */
                int stop = sz;
                for (int q = 0; q < g->nrelocs; q++)
                    if (g->relocs[q].off > k && g->relocs[q].off < stop)
                        stop = g->relocs[q].off;
                ob_str(b, "\t.byte\t");
                for (int j = k; j < stop; j++)
                    ob_fmt(b, "%s%d", j > k ? "," : "",
                           j < g->init_len
                               ? (int)(unsigned char)g->init_bytes[j] : 0);
                ob_str(b, "\n");
                k = stop;
            }
        } else {
            ob_fmt(b, "%s\t.balign\t%d\n\t.type\t%s, %sobject\n%s:\n"
                      "\t.zero\t%d\n",
                   g->in_rodata ? "\t.section\t.rodata\n" : "\t.bss\n",
                   al, asym(g->name), type_sigil(), asym(g->name), sz);
        }
        ob_fmt(b, "\t.size\t%s, %d\n", asym(g->name), sz);
    }

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
    static const char *const arr[2] = { ".init_array", ".fini_array" };
    for (int pass = 0; pass < 2; pass++) {
        int any = 0;
        for (struct func *f = u->funcs; f; f = f->next) {
            if (f->absorbed || !f->has_defn)
                continue;
            if (!(pass == 0 ? f->is_ctor : f->is_dtor))
                continue;
            if (!any) {
                ob_fmt(b, "\n\t.section\t%s,\"aw\",%s%s\n\t.balign\t%d\n",
                       arr[pass], type_sigil(), arr[pass] + 1,
                       target_ptr_size());
                any = 1;
            }
            ptr_slot(b, asym(f->name), 0, 1);
        }
    }
}
