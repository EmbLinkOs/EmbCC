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

/* Thumb, RISC-V and AVR: the file-scope asm blocks and naked functions,
 * with their labels and relocations (at the end of this file). */
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
        switch (kind) {
        case RK_CALL:        return "R_MIPS_26";       /* jal and j alike */
        case RK_MIPS_TEXT26: return "R_MIPS_26";
        case RK_MIPS_HI16:   return "R_MIPS_HI16";
        case RK_MIPS_LO16:   return "R_MIPS_LO16";
        case RK_ABS32:       return "R_MIPS_32";
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
 * the -4 that corrects for that is x86's alone. */
static long addend_for(int kind)
{
    if (kind == RK_ABS64 || kind == RK_ABS32)
        return 0;
    return target_get() == TARGET_X86_64 ? -4 : 0;
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
        ob_fmt(b, ".Ltopasm%d:\n", nb);
        for (long k = 0; k <= ta->codelen; k++) {
            for (int j = 0; j < ta->nsyms; j++) {
                const struct asmsym *as = &ta->syms[j];
                if (as->off != k)
                    continue;
                if (as->is_global)
                    ob_fmt(b, "\t.%s\t%s\n", as->is_weak ? "weak" : "globl",
                           asym(as->name));
                if (as->type == ASMSYM_FUNC || as->type == ASMSYM_OBJECT)
                    ob_fmt(b, "\t.type\t%s, @%s\n", asym(as->name),
                           as->type == ASMSYM_FUNC ? "function" : "object");
                ob_fmt(b, "%s:\n", asym(as->name));
            }
            if (k == ta->codelen)
                break;
            const struct asmrel *r = NULL;
            for (int j = 0; j < ta->nrels; j++)
                if (ta->rels[j].off == k)
                    r = &ta->rels[j];
            if (r && k + 4 <= ta->codelen) {
                const unsigned char *q = text + ta->text_off + k;
                unsigned long w = (unsigned long)q[0] |
                                  ((unsigned long)q[1] << 8) |
                                  ((unsigned long)q[2] << 16) |
                                  ((unsigned long)q[3] << 24);
                int op = (int)(w >> 26), rs = (int)(w >> 21) & 31,
                    rt = (int)(w >> 16) & 31;
                char sym[200];
                if (r->target)
                    snprintf(sym, sizeof sym, "%s%+ld", asym(r->target),
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
                ob_fmt(b, "\t.size\t%s, %ld\n", asym(ta->syms[j].name),
                       ta->syms[j].size);
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
    /* MIPS: the code is already scheduled -- its delay slots are filled --
     * and uses $at itself, so the assembler may neither reorder, nor fill
     * a slot, nor expand a macro through $at. */
    if (target_get() == TARGET_MIPS32)
        ob_str(b, "\t.set\tnoreorder\n\t.set\tnoat\n\t.set\tnomacro\n");
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
            /* MIPS: a relocated instruction is written symbolically --
             * `jal f`, `lui $2, %hi(g)`, `addiu $2, $2, %lo(g)` -- because
             * llvm-mc's MIPS .reloc knows none of R_MIPS_26, HI16 or LO16.
             * Each of these has exactly one encoding, so the assembler has
             * nothing to choose, and a REL assembler stores the addend in
             * the field as EmbCC's object writer does. */
            if (st && target_get() == TARGET_MIPS32 && len == 4) {
                unsigned long w = (unsigned long)text[pc] |
                                  ((unsigned long)text[pc + 1] << 8) |
                                  ((unsigned long)text[pc + 2] << 16) |
                                  ((unsigned long)text[pc + 3] << 24);
                int op = (int)(w >> 26), rs = (int)(w >> 21) & 31,
                    rt = (int)(w >> 16) & 31;
                char sym[200];
                long add = st->addend;
                if (st->kind == RK_MIPS_TEXT26) {
                    snprintf(sym, sizeof sym, "%s", asym(f->name));
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
                else if (st->kind == RK_MIPS_LO16 && op == 0x09)
                    ob_fmt(b, "\taddiu\t$%d, $%d, %%lo(%s%+ld)\n", rt, rs,
                           sym, add);
                else
                    diag_fatal(srcname, 0, "-S cannot spell a MIPS "
                               "relocation of kind %d on instruction "
                               "0x%08lx", st->kind, w);
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
    if (target_get() == TARGET_MIPS32 && u->topasm) {
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
        /* the object's .rodata is 16-aligned (main.c); an assembler's
         * starts at 1 unless told */
        if (target_get() == TARGET_MIPS32)
            ob_str(b, "\t.p2align\t4\n");
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
        if (g->is_common) {           /* -fcommon: the linker places it */
            ob_fmt(b, "\t.comm\t%s,%d,%d\n", asym(g->name), sz,
                   g->user_align > al ? g->user_align : al);
            continue;
        }
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

static int blocks_by_reloc(void)
{
    enum target_arch a = target_get();
    return a == TARGET_THUMB || a == TARGET_RISCV32 ||
           a == TARGET_RISCV64 || a == TARGET_AVR || a == TARGET_LOONGARCH64;
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
             t == R_ARM_THM_MOVT_ABS))
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
    out->sym = asym(r->target);
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
        long bit = target_get() == TARGET_THUMB &&
                   as->type == ASMSYM_FUNC &&
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
    int thumb = target_get() == TARGET_THUMB;
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
                l->name = asym(as->name);
            } else if (ct) {
                l->sym = 1;
                l->ctype = ct;
                l->name = asym(as->name);
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
