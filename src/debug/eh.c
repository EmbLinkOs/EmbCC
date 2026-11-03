/* Unwind and exception tables (eh.h): a CIE (two when some function has
 * exception regions: one naming the personality routine), an FDE per
 * function, and an LSDA per function with regions. */
#include "eh.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"
#include "../arch/target.h"

/* DWARF call frame instructions (DWARF 4, 6.4.2) */
#define CFA_advance_loc   0x40
#define CFA_offset        0x80
#define CFA_advance_loc1  0x02
#define CFA_advance_loc2  0x03
#define CFA_advance_loc4  0x04
#define CFA_def_cfa       0x0c
#define CFA_def_cfa_register 0x0d
#define CFA_def_cfa_offset 0x0e
#define CFA_nop           0x00

/* pointer encodings (the LSB Core specification's DW_EH_PE_*) */
#define EH_PE_uleb128      0x01
#define EH_PE_pcrel_sdata4 0x1b     /* DW_EH_PE_pcrel | DW_EH_PE_sdata4 */
/* ... and the same, one level of indirection away: the slot holds the
 * ADDRESS of a pointer to the object rather than the object. Darwin's
 * type table is written this way so a typeinfo living in another dylib
 * can be named at all, and its linker only offers a relocation for the
 * indirect form (ARM64_RELOC_POINTER_TO_GOT). */
#define EH_PE_indirect_pcrel_sdata4 0x9b
#define EH_PE_omit         0xff

static void need(struct eh_buf *b, int n)
{
    if (b->len + n <= b->cap)
        return;
    while (b->len + n > b->cap)
        b->cap = b->cap ? b->cap * 2 : 256;
    b->p = xrealloc(b->p, (size_t)b->cap);
}

static void u8(struct eh_buf *b, unsigned v)
{
    need(b, 1);
    b->p[b->len++] = (unsigned char)v;
}

/* Fields in the target's byte order (an unwinder reads them natively). */
static void u32(struct eh_buf *b, unsigned long v)
{
    need(b, 4);
    target_put_uint(b->p + b->len, 4, v & 0xffffffffUL);
    b->len += 4;
}

static void patch32(struct eh_buf *b, int at, unsigned long v)
{
    target_put_uint(b->p + at, 4, v & 0xffffffffUL);
}

static void uleb(struct eh_buf *b, unsigned long v)
{
    do {
        unsigned x = v & 0x7f;
        v >>= 7;
        u8(b, v ? x | 0x80 : x);
    } while (v);
}

static void sleb(struct eh_buf *b, long v)
{
    for (;;) {
        unsigned x = (unsigned)(v & 0x7f);
        v >>= 7;                      /* arithmetic: the sign stays */
        if ((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40))) {
            u8(b, x);
            return;
        }
        u8(b, x | 0x80);
    }
}

static int uleb_size(unsigned long v)
{
    int n = 0;
    do {
        v >>= 7;
        n++;
    } while (v);
    return n;
}

static void append(struct eh_buf *to, const struct eh_buf *from)
{
    need(to, from->len);
    memcpy(to->p + to->len, from->p, (size_t)from->len);
    to->len += from->len;
}

/* A 4-byte PC-relative field at the end of the frame or LSDA buffer. */
static void reloc(struct eh_out *o, int in_lsda, int target, long addend,
                  struct global *g)
{
    if (o->nrelocs == o->reloccap) {
        o->reloccap = o->reloccap ? o->reloccap * 2 : 16;
        o->relocs = xrealloc(o->relocs,
                             (size_t)o->reloccap * sizeof *o->relocs);
    }
    struct eh_buf *b = in_lsda == 2 ? &o->pdata : in_lsda ? &o->lsda
                                                          : &o->frame;
    struct eh_reloc *r = &o->relocs[o->nrelocs++];
    r->in_lsda = in_lsda;
    r->off = b->len;
    r->target = target;
    r->addend = addend;
    r->glob = g;
    r->kind = EHR_PREL32;
    u32(b, 0);                        /* the linker writes it */
}

/* Pad an entry started at `start` (its length field) to 8 bytes with
 * DW_CFA_nop, then write its length. */
static void close_entry(struct eh_buf *b, int start)
{
    while ((b->len - start) % 8)
        u8(b, CFA_nop);
    patch32(b, start, (unsigned long)(b->len - start - 4));
}

/* Advance the location from *at to `to` bytes into the function. */
static void advance(struct eh_buf *b, int *at, int to, int code_align)
{
    unsigned long d = (unsigned long)(to - *at) / (unsigned long)code_align;
    if (d == 0)
        return;
    if (d < 64)
        u8(b, CFA_advance_loc | (unsigned)d);
    else if (d < 256) {
        u8(b, CFA_advance_loc1);
        u8(b, (unsigned)d);
    } else if (d < 65536) {
        u8(b, CFA_advance_loc2);
        need(b, 2);
        target_put_uint(b->p + b->len, 2, d);
        b->len += 2;
    } else {
        u8(b, CFA_advance_loc4);
        u32(b, d);
    }
    *at = to;
}

/* ---- LSDAs ---- */

static int type_index(struct ir_func *fn, struct global *ti)
{
    for (int i = 0; i < fn->neh_types; i++)
        if (fn->eh_types[i] == ti)
            return i + 1;
    return 0;
}

/* Region r's action chain in the action table `at`: its own actions,
 * then (sharing their records) its enclosing regions'. The offset of
 * its first record, memoized in first[] (-1: not yet written). */
static int chain(struct ir_func *fn, int r, struct eh_buf *at, int *first)
{
    if (first[r] >= 0)
        return first[r];
    struct ir_eh *h = &fn->eh[r];
    int parent = h->parent >= 0 ? chain(fn, h->parent, at, first) : -1;
    if (h->nacts == 0)
        return first[r] = parent;
    first[r] = at->len;
    for (int k = 0; k < h->nacts; k++) {
        struct eh_act *a = &h->acts[k];
        sleb(at, a->cleanup ? 0 : type_index(fn, a->ti));
        int here = at->len;          /* the displacement field */
        if (k + 1 < h->nacts)
            sleb(at, 1);             /* the next record follows it */
        else
            sleb(at, parent >= 0 ? parent - here : 0);
    }
    return first[r];
}

static void emit_lsda(struct eh_out *o, struct ir_func *fn)
{
    struct eh_buf cs = { 0, 0, 0 }, at = { 0, 0, 0 };
    int *first = xmalloc((size_t)fn->neh * sizeof *first);
    for (int r = 0; r < fn->neh; r++)
        first[r] = -1;
    /* every call: its landing pad and chain, or none (unwinding goes on);
     * neighbours alike share a record */
    for (int i = 0; i < fn->ncsites;) {
        struct ir_csite *c = &fn->csites[i];
        int lp = 0, act = 0;
        if (c->region >= 0) {
            lp = fn->eh[c->region].lp_off;
            int f = chain(fn, c->region, &at, first);
            act = f >= 0 ? f + 1 : 0;
        }
        int start = c->start, end = c->end, k = i + 1;
        while (k < fn->ncsites && fn->csites[k].region == c->region) {
            end = fn->csites[k].end;
            k++;
        }
        uleb(&cs, (unsigned long)start);
        uleb(&cs, (unsigned long)(end - start));
        uleb(&cs, (unsigned long)lp);
        uleb(&cs, (unsigned long)act);
        i = k;
    }
    free(first);
    u8(&o->lsda, EH_PE_omit);                 /* landing pads: from the
                                               * function's start */
    /* Win64 (MinGW's libgcc, as clang writes it): absolute eight-byte
     * typeinfo addresses, the image relocated as a whole */
    int win = target_os_get() == TGT_OS_WINDOWS;
    if (fn->neh_types) {
        u8(&o->lsda, win ? 0x00                /* DW_EH_PE_absptr */
                     : target_os_get() == TGT_OS_DARWIN
                     ? EH_PE_indirect_pcrel_sdata4
                     : EH_PE_pcrel_sdata4);    /* the type table's */
        uleb(&o->lsda, (unsigned long)(1 + uleb_size((unsigned long)cs.len) +
                                       cs.len + at.len +
                                       (win ? 8 : 4) * fn->neh_types));
    } else {
        u8(&o->lsda, EH_PE_omit);
    }
    u8(&o->lsda, EH_PE_uleb128);              /* the call-site table's */
    uleb(&o->lsda, (unsigned long)cs.len);
    append(&o->lsda, &cs);
    append(&o->lsda, &at);
    /* the types, index n first: entry i sits 4*i bytes before the base */
    for (int i = fn->neh_types; i >= 1; i--) {
        struct global *g = fn->eh_types[i - 1];
        if (g && win) {
            reloc(o, 1, EHT_GLOBAL, 0, g);
            o->relocs[o->nrelocs - 1].kind = EHR_ABS64;
            u32(&o->lsda, 0);                 /* reloc() wrote four */
        } else if (g) {
            reloc(o, 1, EHT_GLOBAL, 0, g);
        } else {
            u32(&o->lsda, 0);                 /* catch (...) */
            if (win)
                u32(&o->lsda, 0);
        }
    }
    free(cs.p);
    free(at.p);
}

/* ---- .eh_frame ---- */

/* A CIE; with_lsda names the personality routine and says FDEs carry an
 * LSDA pointer ("zPLR"), else "zR". */
static int emit_cie(struct eh_out *o, int arm64, int with_lsda)
{
    struct eh_buf *b = &o->frame;
    int ra = arm64 ? 30 : 16;         /* the return address's column */
    int cie = b->len;
    u32(b, 0);                        /* length, patched */
    u32(b, 0);                        /* CIE id */
    u8(b, 1);                         /* version */
    u8(b, 'z');
    if (with_lsda) {
        u8(b, 'P');
        u8(b, 'L');
    }
    u8(b, 'R');
    u8(b, 0);
    uleb(b, arm64 ? 4 : 1);           /* code alignment */
    sleb(b, -8);                      /* data alignment */
    uleb(b, (unsigned long)ra);
    if (with_lsda) {
        uleb(b, 7);                   /* augmentation data: */
        u8(b, EH_PE_pcrel_sdata4);    /* ... the personality, */
        reloc(o, 0, EHT_PERSONALITY, 0, NULL);
        u8(b, EH_PE_pcrel_sdata4);    /* ... the LSDA pointers', */
    } else {
        uleb(b, 1);
    }
    u8(b, EH_PE_pcrel_sdata4);        /* ... the FDE pointers' encodings */
    /* at a function's entry the CFA is the stack pointer (+8 on x86-64,
     * past the return address the call pushed) */
    u8(b, CFA_def_cfa);
    uleb(b, arm64 ? 31 : 7);
    uleb(b, arm64 ? 0 : 8);
    if (!arm64) {
        u8(b, CFA_offset | (unsigned)ra);    /* return address at CFA-8 */
        uleb(b, 1);
    }
    close_entry(b, cie);
    return cie;
}

void eh_emit(struct ir_unit *iu, int arm64, struct eh_out *out)
{
    int code_align = arm64 ? 4 : 1;
    int ra = arm64 ? 30 : 16;
    int fp = arm64 ? 29 : 6;
    struct eh_buf *b = &out->frame;

    int cie = emit_cie(out, arm64, 0), cie_lsda = -1;
    for (int n = 0; n < iu->nfuncs; n++)
        if (iu->funcs[n].neh && cie_lsda < 0)
            cie_lsda = emit_cie(out, arm64, 1);

    for (int n = 0; n < iu->nfuncs; n++) {
        struct ir_func *fn = &iu->funcs[n];
        struct func *f = fn->src;
        if (!f || f->absorbed || !f->has_defn || f->code_len <= 0)
            continue;
        int lsda = -1;
        if (fn->neh) {
            lsda = out->lsda.len;
            emit_lsda(out, fn);
        }
        if (out->nfuncs == out->funccap) {
            out->funccap = out->funccap ? out->funccap * 2 : 16;
            out->funcs = xrealloc(out->funcs,
                                  (size_t)out->funccap * sizeof *out->funcs);
        }
        out->funcs[out->nfuncs].code_off = f->code_off;
        out->funcs[out->nfuncs].code_len = f->code_len;
        out->funcs[out->nfuncs].lsda_off = lsda;
        out->nfuncs++;

        int fde = b->len;
        u32(b, 0);                    /* length, patched */
        u32(b, (unsigned long)(b->len - (lsda >= 0 ? cie_lsda : cie)));
        reloc(out, 0, EHT_TEXT, f->code_off, NULL);        /* pc begin */
        u32(b, (unsigned long)f->code_len);                /* pc range */
        if (lsda >= 0) {
            uleb(b, 4);               /* augmentation data: the LSDA */
            reloc(out, 0, EHT_LSDA, lsda, NULL);
        } else {
            uleb(b, 0);
        }
        int at = 0;
        /* pac-ret: from after paciasp the saved return address is signed,
         * and an unwinder strips it (DW_CFA_AARCH64_negate_ra_state) */
        if (arm64 && f->cfi_pac) {
            advance(b, &at, f->cfi_pac, code_align);
            u8(b, 0x2d);
        }
        if (f->cfi_frameless) {
            /* Nothing to say: the CIE's opening rule already describes
             * this function from entry to return. */
            close_entry(b, fde);
            continue;
        }
        if (f->cfi_pushonly) {
            /* No frame pointer: each push moves the CFA eight further
             * from rsp, and the saved registers are where they went. */
            for (int k = 0; k < f->cfi_npush; k++) {
                advance(b, &at, f->cfi_push_end[k], code_align);
                u8(b, CFA_def_cfa_offset);
                uleb(b, (unsigned long)(16 + 8 * k));
            }
            for (int k = 0; k < f->cfi_nsaved; k++) {
                u8(b, CFA_offset | (unsigned)f->cfi_reg[k]);
                uleb(b, (unsigned long)(-f->cfi_off[k] / 8));
            }
            close_entry(b, fde);
            continue;
        }
        /* the frame record pushed: the CFA 16 above the stack pointer,
         * the caller's frame pointer (and on aarch64 x30) below it */
        advance(b, &at, f->cfi_push, code_align);
        u8(b, CFA_def_cfa_offset);
        uleb(b, 16);
        u8(b, CFA_offset | (unsigned)fp);
        uleb(b, 2);
        if (arm64) {
            u8(b, CFA_offset | (unsigned)ra);
            uleb(b, 1);
        }
        /* the frame pointer set: the CFA 16 above it from here on */
        advance(b, &at, f->cfi_frame, code_align);
        u8(b, CFA_def_cfa_register);
        uleb(b, (unsigned long)fp);
        if (f->cfi_nsaved) {
            advance(b, &at, f->cfi_saved_at, code_align);
            for (int k = 0; k < f->cfi_nsaved; k++) {
                u8(b, CFA_offset | (unsigned)f->cfi_reg[k]);
                uleb(b, (unsigned long)(-f->cfi_off[k] / 8));
            }
        }
        close_entry(b, fde);
    }
    /* no zero terminator: the linked image's .eh_frame ends with one */
}

/* ---- .eh_frame from the backend's CFI records ----------------------
 *
 * The other backends record each prologue's steps as ir_cfi entries
 * (ir_cfi_add) -- the same ones .debug_frame is made from (dwarf.c) --
 * so their .eh_frame is that program, in .eh_frame's dress: a "zR" or
 * "zPLR" CIE with PC-relative pointers, code alignment 1 and data
 * alignment -1 (every offset in bytes), and an FDE per function with its
 * LSDA when it has exception regions. At entry the CFA is the stack
 * pointer and the return address is in its register (frame_sp,
 * frame_ra, the backend registry's). */
static int emit_cie_cfi(struct eh_out *o, int sp, int ra, int with_lsda)
{
    struct eh_buf *b = &o->frame;
    int cie = b->len;
    u32(b, 0);
    u32(b, 0);
    u8(b, 1);
    u8(b, 'z');
    if (with_lsda) {
        u8(b, 'P');
        u8(b, 'L');
    }
    u8(b, 'R');
    u8(b, 0);
    uleb(b, 1);                       /* code alignment */
    sleb(b, -1);                      /* data alignment: bytes */
    uleb(b, (unsigned long)ra);
    if (with_lsda) {
        uleb(b, 7);
        u8(b, EH_PE_pcrel_sdata4);
        reloc(o, 0, EHT_PERSONALITY, 0, NULL);
        u8(b, EH_PE_pcrel_sdata4);
    } else {
        uleb(b, 1);
    }
    u8(b, EH_PE_pcrel_sdata4);
    u8(b, CFA_def_cfa);
    uleb(b, (unsigned long)sp);
    uleb(b, 0);
    close_entry(b, cie);
    return cie;
}

void eh_emit_cfi(struct ir_unit *iu, int sp, int ra, struct eh_out *out)
{
    struct eh_buf *b = &out->frame;
    int cie = emit_cie_cfi(out, sp, ra, 0), cie_lsda = -1;
    for (int n = 0; n < iu->nfuncs; n++)
        if (iu->funcs[n].neh && cie_lsda < 0)
            cie_lsda = emit_cie_cfi(out, sp, ra, 1);
    for (int n = 0; n < iu->nfuncs; n++) {
        struct ir_func *fn = &iu->funcs[n];
        struct func *f = fn->src;
        /* ncfi -1: a function its backend cannot describe (an interrupt
         * handler): no FDE, so a walk stops there rather than mislead */
        if (!f || f->absorbed || !f->has_defn || f->code_len <= 0 ||
            fn->ncfi < 0)
            continue;
        int lsda = -1;
        if (fn->neh) {
            lsda = out->lsda.len;
            emit_lsda(out, fn);
        }
        long lo = f->code_off + f->code_entry;
        int fde = b->len;
        u32(b, 0);
        u32(b, (unsigned long)(b->len - (lsda >= 0 ? cie_lsda : cie)));
        reloc(out, 0, EHT_TEXT, lo, NULL);
        u32(b, (unsigned long)(f->code_len - f->code_entry));
        if (lsda >= 0) {
            uleb(b, 4);
            reloc(out, 0, EHT_LSDA, lsda, NULL);
        } else {
            uleb(b, 0);
        }
        int at = 0;
        for (int k = 0; k < fn->ncfi; k++) {
            const struct ir_cfi *c = &fn->cfi[k];
            advance(b, &at, (int)(c->off - f->code_entry), 1);
            switch (c->kind) {
            case IR_CFI_CFA_OFFSET:
                u8(b, CFA_def_cfa_offset);
                uleb(b, (unsigned long)c->val);
                break;
            case IR_CFI_CFA_REG:
                u8(b, CFA_def_cfa);
                uleb(b, (unsigned long)c->reg);
                uleb(b, (unsigned long)c->val);
                break;
            case IR_CFI_SAVED:
                if (c->val <= 0 && c->reg < 64) {
                    u8(b, CFA_offset | (unsigned)c->reg);
                    uleb(b, (unsigned long)-c->val);
                } else if (c->val <= 0) {
                    u8(b, 0x05);                 /* offset_extended */
                    uleb(b, (unsigned long)c->reg);
                    uleb(b, (unsigned long)-c->val);
                } else {
                    u8(b, 0x11);                 /* offset_extended_sf */
                    uleb(b, (unsigned long)c->reg);
                    sleb(b, -c->val);
                }
                break;
            }
        }
        close_entry(b, fde);
        if (out->nfuncs == out->funccap) {
            out->funccap = out->funccap ? out->funccap * 2 : 16;
            out->funcs = xrealloc(out->funcs,
                                  (size_t)out->funccap * sizeof *out->funcs);
        }
        out->funcs[out->nfuncs].code_off = f->code_off;
        out->funcs[out->nfuncs].code_len = f->code_len;
        out->funcs[out->nfuncs].lsda_off = lsda;
        out->nfuncs++;
    }
}

/* ---- 32-bit ARM: EHABI ---------------------------------------------- */

/* A frame's unwind opcodes (IHI 0038 9.3), from the Thumb prologue its
 * CFI records describe: [16 bytes of varargs] push {regs} [vpush
 * {d8..}] [sub sp, #frame] [mov r7, sp]. Undone in reverse: vsp = r7,
 * vsp += frame, vpop, pop, vsp += 16. The return comes from lr unless pc
 * was popped (the implicit finish). Returns the count. */
static int arm_opcodes(const struct ir_func *fn, unsigned char *op)
{
    unsigned mask = 0;
    int nd = 0, fp = 0, n = 0;
    long total = 0, maxoff = 0x7fffffffL;
    int have_core = 0;
    for (int k = 0; k < fn->ncfi; k++) {
        const struct ir_cfi *c = &fn->cfi[k];
        if (c->kind == IR_CFI_CFA_OFFSET)
            total = c->val;
        else if (c->kind == IR_CFI_CFA_REG) {
            total = c->val;
            fp = c->reg == 7;
        } else if (c->reg < 16) {
            mask |= 1u << c->reg;
            if (!have_core || -c->val < maxoff)
                maxoff = -c->val;           /* the highest slot: nearest */
            have_core = 1;
        } else if (c->reg >= 256 + 8) {
            nd++;
        }
    }
    int npush = 0;
    for (int r = 0; r < 16; r++)
        npush += (mask >> r) & 1;
    /* the varargs block: what lies above the highest pushed register */
    long va = have_core ? maxoff - 4 : 0;
    if (!have_core && fn->ncfi > 0 && fn->cfi[0].kind == IR_CFI_CFA_OFFSET &&
        fn->cfi[0].val == 16 && fn->ncfi > 1)
        va = 16;
    long frame = total - va - 4L * npush - 8L * nd;
    if (fp)
        op[n++] = 0x97;                       /* vsp = r7 */
    while (frame > 0) {
        if (frame >= 0x204 + 4) {
            unsigned long v = (unsigned long)(frame - 0x204) >> 2;
            op[n++] = 0xb2;
            do {
                unsigned char byte = v & 0x7f;
                v >>= 7;
                if (v)
                    byte |= 0x80;
                op[n++] = byte;
            } while (v);
            frame -= 0x204 + (long)(((unsigned long)(frame - 0x204) >> 2) << 2);
            continue;
        }
        long k = frame > 256 ? 256 : frame;
        op[n++] = (unsigned char)((k - 4) >> 2);
        frame -= k;
    }
    if (nd)
        op[n++] = (unsigned char)(0xd0 | (nd - 1));   /* vpop d8..d(7+nd) */
    /* one push stores its registers ascending, so r0-r3 lie below r4 and
     * up: they come off first */
    if (mask & 0xf) {
        op[n++] = 0xb1;
        op[n++] = (unsigned char)(mask & 0xf);
    }
    if (mask & 0xfff0) {
        unsigned m = (mask & 0xfff0) >> 4;
        op[n++] = (unsigned char)(0x80 | (m >> 8));
        op[n++] = (unsigned char)(m & 0xff);
    }
    while (va > 0) {
        long k = va > 256 ? 256 : va;
        op[n++] = (unsigned char)((k - 4) >> 2);
        va -= k;
    }
    return n;
}

static void reloc_kind(struct eh_out *o, int in_lsda, int target, long addend,
                       int kind)
{
    reloc(o, in_lsda, target, addend, NULL);
    o->relocs[o->nrelocs - 1].kind = kind;
}

/* `n` opcodes from op[start..] packed as words, MSB first, finish-padded */
static void opcode_words(struct eh_buf *b, const unsigned char *op, int start,
                         int n)
{
    for (int k = start; k < n; k += 4) {
        unsigned long w = 0;
        for (int j = 0; j < 4; j++)
            w = (w << 8) | (k + j < n ? op[k + j] : 0xb0);
        u32(b, w);
    }
}

void eh_emit_arm(struct ir_unit *iu, struct eh_out *out)
{
    struct eh_buf *x = &out->frame, *t = &out->lsda;
    for (int n = 0; n < iu->nfuncs; n++) {
        struct ir_func *fn = &iu->funcs[n];
        struct func *f = fn->src;
        if (!f || f->absorbed || !f->has_defn || f->code_len <= 0)
            continue;
        reloc_kind(out, 0, EHT_TEXT, f->code_off + f->code_entry, EHR_PREL31);
        if (fn->ncfi < 0) {
            u32(x, 1);                         /* EXIDX_CANTUNWIND */
            continue;
        }
        unsigned char op[64];
        int nop = arm_opcodes(fn, op);
        if (!fn->neh && nop <= 3) {
            /* the short form inline: personality routine 0 */
            unsigned long w = 0x80;
            for (int j = 0; j < 3; j++)
                w = (w << 8) | (j < nop ? op[j] : 0xb0);
            u32(x, w);
            /* the R_ARM_NONE that keeps pr0 in the link, at this word */
            if (out->nrelocs == out->reloccap) {
                out->reloccap = out->reloccap ? out->reloccap * 2 : 16;
                out->relocs = xrealloc(out->relocs, (size_t)out->reloccap *
                                                    sizeof *out->relocs);
            }
            struct eh_reloc *r = &out->relocs[out->nrelocs++];
            memset(r, 0, sizeof *r);
            r->off = x->len - 4;
            r->target = EHT_PR0;
            r->kind = EHR_NONE;
            continue;
        }
        while (t->len % 4)
            u8(t, 0);
        reloc_kind(out, 0, EHT_LSDA, t->len, EHR_PREL31);
        if (fn->neh) {
            /* C++: __gxx_personality_v0, its opcode words, its LSDA */
            reloc_kind(out, 1, EHT_PERSONALITY, 0, EHR_PREL31);
            int nw = nop > 3 ? (nop - 3 + 3) / 4 : 0;
            unsigned long w = (unsigned long)nw << 24;
            for (int j = 0; j < 3; j++)
                w |= (unsigned long)(j < nop ? op[j] : 0xb0) << (16 - 8 * j);
            u32(t, w);
            opcode_words(t, op, 3, nop);
            emit_lsda(out, fn);
        } else {
            /* the long form: personality routine 1 */
            int nw = nop > 2 ? (nop - 2 + 3) / 4 : 0;
            unsigned long w = 0x81000000UL | ((unsigned long)nw << 16);
            w |= (unsigned long)(nop > 0 ? op[0] : 0xb0) << 8;
            w |= (unsigned long)(nop > 1 ? op[1] : 0xb0);
            u32(t, w);
            opcode_words(t, op, 2, nop);
            if (out->nrelocs == out->reloccap) {
                out->reloccap = out->reloccap ? out->reloccap * 2 : 16;
                out->relocs = xrealloc(out->relocs, (size_t)out->reloccap *
                                                    sizeof *out->relocs);
            }
            struct eh_reloc *r = &out->relocs[out->nrelocs++];
            memset(r, 0, sizeof *r);
            r->off = x->len - 4;
            r->target = EHT_PR1;
            r->kind = EHR_NONE;
        }
        while (t->len % 4)
            u8(t, 0);
    }
}

/* ---- Win64: .xdata and .pdata ---------------------------------------- */

void eh_emit_win64(struct ir_unit *iu, struct eh_out *out)
{
    struct eh_buf *x = &out->frame;
    for (int n = 0; n < iu->nfuncs; n++) {
        struct ir_func *fn = &iu->funcs[n];
        struct func *f = fn->src;
        if (!f || f->absorbed || !f->has_defn || f->code_len <= 0 ||
            !f->win_uw)
            continue;
        while (x->len & 3)
            u8(x, 0);
        int xoff = x->len;
        for (int k = 0; k < f->win_uw_len; k++)
            u8(x, f->win_uw[k]);
        if (fn->neh) {
            /* UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER: the personality's
             * RVA, then its data -- the LSDA, which MinGW's
             * _GCC_specific_handler finds right there (HandlerData) */
            x->p[xoff] |= 3 << 3;
            reloc_kind(out, 0, EHT_PERSONALITY, 0, EHR_RVA32);
            int lbase = x->len, l0 = out->lsda.len, r0 = out->nrelocs;
            emit_lsda(out, fn);
            for (int k = l0; k < out->lsda.len; k++)
                u8(x, out->lsda.p[k]);
            for (int k = r0; k < out->nrelocs; k++)
                if (out->relocs[k].in_lsda == 1) {
                    out->relocs[k].in_lsda = 0;
                    out->relocs[k].off += lbase - l0;
                }
            out->lsda.len = l0;
        }
        long lo = f->code_off + f->code_entry;
        reloc_kind(out, 2, EHT_TEXT, lo, EHR_RVA32);
        reloc_kind(out, 2, EHT_TEXT, f->code_off + f->code_len, EHR_RVA32);
        reloc_kind(out, 2, EHT_XDATA, xoff, EHR_RVA32);
    }
}

void eh_free(struct eh_out *out)
{
    free(out->frame.p);
    free(out->pdata.p);
    free(out->lsda.p);
    free(out->relocs);
    free(out->funcs);
    memset(out, 0, sizeof *out);
}
