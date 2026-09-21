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
 * like assembly and is not (docs/language/compatibility.md's rule:
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

/* One place in .text that names a symbol. Collected from what the backend
 * recorded, so this is the same information the ELF writer turns into
 * relocations — not a second derivation of it. */
struct site {
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
static const char *reloc_name(int kind)
{
    switch (kind) {
    case RK_CALL:        return "R_X86_64_PLT32";
    case RK_PCREL32:     return "R_X86_64_PC32";
    case RK_ABS64:       return "R_X86_64_64";
    case RK_ABS32:       return "R_X86_64_32";
    case RK_DATA_PREL32: return "R_X86_64_PC32";
    default:             return "R_X86_64_PC32";
    }
}

static long addend_for(int kind)
{
    return kind == RK_ABS64 || kind == RK_ABS32 ? 0 : -4;
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
        site[ns].name = ext[i].callee ? ext[i].callee->name : "?";
        site[ns++].kind = 0;
    }
    for (int i = 0; i < nfs; i++) {
        site[ns].off = fs[i].patch_off;
        site[ns].name = fs[i].target ? fs[i].target->name : "?";
        site[ns++].kind = fs[i].kind;
    }
    for (int i = 0; i < ngs; i++) {
        site[ns].off = gs[i].patch_off;
        site[ns].name = gs[i].glob ? gs[i].glob->name : "?";
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
    ob_str(b, "\t.text\n");
    long prev_end = 0;            /* where the previous function's code ended */

    for (int i = 0; i < iu->nfuncs; i++) {
        struct ir_func *f = &iu->funcs[i];
        if (!f->src || f->src->code_len <= 0)
            continue;
        long lo = f->src->code_off, hi = lo + f->src->code_len;
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
            ob_fmt(b, "\t.globl\t%s\n", f->name);
        ob_fmt(b, "\t.type\t%s, @function\n%s:\n", f->name, f->name);

        long pc = lo;
        while (pc < hi) {
            char dis[256];
            int len = embdbg_decode_one(text + pc, (int)(hi - pc),
                                        (unsigned long)pc, dis);
            if (len < 1)
                len = 1;
            const struct site *st = site_in(site, ns, (int)pc,
                                            (int)(pc + len));
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
            ob_fmt(b, "\t# %s\n", dis);
            /* A relocation is attached explicitly rather than spelled into
             * the instruction. Written symbolically, an assembler that can
             * SEE the target resolves it itself -- `lea add(%rip)` becomes a
             * fixed displacement and no relocation at all -- which is a
             * correct program but not the same object. `.reloc` says where
             * and against what, and leaves the bytes alone. */
            if (st) {
                char sym[160];
                if (st->name)
                    snprintf(sym, sizeof sym, "%s", st->name);
                else
                    str_label(sym, sizeof sym, iu, st->str_off);
                const char *rt = reloc_name(st->kind);
                long field = (long)st->off - pc;   /* within the instruction */
                long tail = len - field - (st->kind == RK_ABS64 ? 8 : 4);
                ob_fmt(b, "\t.reloc\t.-%ld, %s, %s%+ld\n",
                       len - field, rt, sym, addend_for(st->kind));
                (void)tail;
            }
            pc += len;
        }
        ob_fmt(b, "\t.size\t%s, .-%s\n", f->name, f->name);
        prev_end = hi;
    }
    /* .text may end with padding too. */
    if (textlen > prev_end) {
        ob_str(b, "\t.byte\t");
        for (long k = prev_end; k < textlen; k++)
            ob_fmt(b, "%s0x%02x", k > prev_end ? "," : "",
                   (unsigned)text[k]);
        ob_str(b, "\n");
    }

    /* .rodata: the string literals, each under the label the code refers
     * to. Emitted as bytes, because a string may hold anything. */
    if (rodata && iu->rodata_len > 0) {
        ob_str(b, "\n\t.section\t.rodata\n");
        for (int i = 0; i < iu->nstrs; i++) {
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
        int sz = g->ty ? ty_size(g->ty) : 0;
        int al = g->ty ? ty_align(g->ty) : 1;
        if (!sz)
            continue;
        if (!any_data) { ob_str(b, "\n"); any_data = 1; }
        if (!g->is_static)
            ob_fmt(b, "\t.globl\t%s\n", g->name);
        if (g->init_bytes && g->init_len > 0) {
            ob_str(b, "\t.data\n");
            ob_fmt(b, "\t.align\t%d\n\t.type\t%s, @object\n%s:\n",
                   al, g->name, g->name);
            /* A pointer slot in an initializer is an ADDRESS the linker
             * fills in, not bytes: `static char *p = "hi";` holds a
             * relocation, and emitting its zeroed bytes would produce a
             * null pointer that links and then crashes. The byte runs
             * between relocations are emitted as bytes; each relocated
             * slot becomes a `.quad symbol + addend`. */
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
                        snprintf(sym, sizeof sym, "%s", r->gtarget->name);
                    else if (r->ftarget)
                        snprintf(sym, sizeof sym, "%s", r->ftarget->name);
                    else
                        snprintf(sym, sizeof sym, "0");
                    ob_fmt(b, "\t.quad\t%s", sym);
                    if (r->addend)
                        ob_fmt(b, "%+ld", r->addend);
                    ob_str(b, "\n");
                    k += 8;
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
            ob_fmt(b, "\t.bss\n\t.align\t%d\n\t.type\t%s, @object\n%s:\n"
                      "\t.zero\t%d\n", al, g->name, g->name, sz);
        }
        ob_fmt(b, "\t.size\t%s, %d\n", g->name, sz);
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
                ob_fmt(b, "\n\t.section\t%s,\"aw\",@%s\n\t.align\t8\n",
                       arr[pass], arr[pass] + 1);
                any = 1;
            }
            ob_fmt(b, "\t.quad\t%s\n", f->name);
        }
    }
}
