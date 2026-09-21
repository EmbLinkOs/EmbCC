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

static void u32(struct eh_buf *b, unsigned long v)
{
    for (int i = 0; i < 4; i++)
        u8(b, (unsigned)(v >> (8 * i)) & 0xff);
}

static void patch32(struct eh_buf *b, int at, unsigned long v)
{
    for (int i = 0; i < 4; i++)
        b->p[at + i] = (unsigned char)((v >> (8 * i)) & 0xff);
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
    struct eh_buf *b = in_lsda ? &o->lsda : &o->frame;
    struct eh_reloc *r = &o->relocs[o->nrelocs++];
    r->in_lsda = in_lsda;
    r->off = b->len;
    r->target = target;
    r->addend = addend;
    r->glob = g;
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
        u8(b, (unsigned)d & 0xff);
        u8(b, (unsigned)(d >> 8));
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
    if (fn->neh_types) {
        u8(&o->lsda, target_os_get() == TGT_OS_DARWIN
                     ? EH_PE_indirect_pcrel_sdata4
                     : EH_PE_pcrel_sdata4);    /* the type table's */
        uleb(&o->lsda, (unsigned long)(1 + uleb_size((unsigned long)cs.len) +
                                       cs.len + at.len + 4 * fn->neh_types));
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
        if (g)
            reloc(o, 1, EHT_GLOBAL, 0, g);
        else
            u32(&o->lsda, 0);                 /* catch (...) */
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

void eh_free(struct eh_out *out)
{
    free(out->frame.p);
    free(out->lsda.p);
    free(out->relocs);
    free(out->funcs);
    memset(out, 0, sizeof *out);
}
