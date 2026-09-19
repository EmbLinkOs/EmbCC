/* Unwind tables: one CIE, then an FDE per function (eh.h). */
#include "eh.h"

#include <stdlib.h>
#include <string.h>

#include "../driver/util.h"

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

#define EH_PE_pcrel_sdata4 0x1b     /* DW_EH_PE_pcrel | DW_EH_PE_sdata4 */

static void need(struct eh_out *o, int n)
{
    if (o->framelen + n <= o->framecap)
        return;
    while (o->framelen + n > o->framecap)
        o->framecap = o->framecap ? o->framecap * 2 : 256;
    o->frame = xrealloc(o->frame, (size_t)o->framecap);
}

static void u8(struct eh_out *o, unsigned v)
{
    need(o, 1);
    o->frame[o->framelen++] = (unsigned char)v;
}

static void u32(struct eh_out *o, unsigned long v)
{
    for (int i = 0; i < 4; i++)
        u8(o, (unsigned)(v >> (8 * i)) & 0xff);
}

static void patch32(struct eh_out *o, int at, unsigned long v)
{
    for (int i = 0; i < 4; i++)
        o->frame[at + i] = (unsigned char)((v >> (8 * i)) & 0xff);
}

static void uleb(struct eh_out *o, unsigned long v)
{
    do {
        unsigned b = v & 0x7f;
        v >>= 7;
        u8(o, v ? b | 0x80 : b);
    } while (v);
}

static void sleb(struct eh_out *o, long v)
{
    for (;;) {
        unsigned b = (unsigned)(v & 0x7f);
        v >>= 7;                      /* arithmetic: the sign stays */
        if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40))) {
            u8(o, b);
            return;
        }
        u8(o, b | 0x80);
    }
}

static void reloc(struct eh_out *o, int target, long addend)
{
    if (o->nrelocs == o->reloccap) {
        o->reloccap = o->reloccap ? o->reloccap * 2 : 16;
        o->relocs = xrealloc(o->relocs,
                             (size_t)o->reloccap * sizeof *o->relocs);
    }
    o->relocs[o->nrelocs].off = o->framelen;
    o->relocs[o->nrelocs].target = target;
    o->relocs[o->nrelocs].addend = addend;
    o->nrelocs++;
    u32(o, 0);                        /* the linker writes it */
}

/* Pad an entry started at `start` (its length field) to 8 bytes with
 * DW_CFA_nop, then write its length. */
static void close_entry(struct eh_out *o, int start)
{
    while ((o->framelen - start) % 8)
        u8(o, CFA_nop);
    patch32(o, start, (unsigned long)(o->framelen - start - 4));
}

/* Advance the location from *at to `to` bytes into the function. */
static void advance(struct eh_out *o, int *at, int to, int code_align)
{
    unsigned long d = (unsigned long)(to - *at) / (unsigned long)code_align;
    if (d == 0)
        return;
    if (d < 64)
        u8(o, CFA_advance_loc | (unsigned)d);
    else if (d < 256) {
        u8(o, CFA_advance_loc1);
        u8(o, (unsigned)d);
    } else if (d < 65536) {
        u8(o, CFA_advance_loc2);
        u8(o, (unsigned)d & 0xff);
        u8(o, (unsigned)(d >> 8));
    } else {
        u8(o, CFA_advance_loc4);
        u32(o, d);
    }
    *at = to;
}

void eh_emit(struct ir_unit *iu, int arm64, struct eh_out *out)
{
    int code_align = arm64 ? 4 : 1;
    int ra = arm64 ? 30 : 16;         /* the return address's column */
    int sp = arm64 ? 31 : 7;
    int fp = arm64 ? 29 : 6;

    /* the CIE: at a function's entry the CFA is the stack pointer (+8 on
     * x86-64, past the return address the call pushed, which is there) */
    int cie = out->framelen;
    u32(out, 0);                      /* length, patched */
    u32(out, 0);                      /* CIE id */
    u8(out, 1);                       /* version */
    u8(out, 'z');
    u8(out, 'R');
    u8(out, 0);
    uleb(out, (unsigned long)code_align);
    sleb(out, -8);                    /* data alignment */
    uleb(out, (unsigned long)ra);
    uleb(out, 1);                     /* augmentation data: */
    u8(out, EH_PE_pcrel_sdata4);      /* ... FDE pointers' encoding */
    u8(out, CFA_def_cfa);
    uleb(out, (unsigned long)sp);
    uleb(out, arm64 ? 0 : 8);
    if (!arm64) {
        u8(out, CFA_offset | (unsigned)ra);  /* return address at CFA-8 */
        uleb(out, 1);
    }
    close_entry(out, cie);

    for (int n = 0; n < iu->nfuncs; n++) {
        struct func *f = iu->funcs[n].src;
        if (!f || f->absorbed || !f->has_defn || f->code_len <= 0)
            continue;
        int fde = out->framelen;
        u32(out, 0);                  /* length, patched */
        u32(out, (unsigned long)(out->framelen - cie));   /* CIE pointer */
        reloc(out, EHT_TEXT, f->code_off);                /* pc begin */
        u32(out, (unsigned long)f->code_len);             /* pc range */
        uleb(out, 0);                 /* no augmentation data */
        int at = 0;
        /* the frame record pushed: the CFA 16 above the stack pointer,
         * the caller's frame pointer (and on aarch64 x30) below it */
        advance(out, &at, f->cfi_push, code_align);
        u8(out, CFA_def_cfa_offset);
        uleb(out, 16);
        u8(out, CFA_offset | (unsigned)fp);
        uleb(out, 2);
        if (arm64) {
            u8(out, CFA_offset | (unsigned)ra);
            uleb(out, 1);
        }
        /* the frame pointer set: the CFA 16 above it from here on */
        advance(out, &at, f->cfi_frame, code_align);
        u8(out, CFA_def_cfa_register);
        uleb(out, (unsigned long)fp);
        if (f->cfi_nsaved) {
            advance(out, &at, f->cfi_saved_at, code_align);
            for (int k = 0; k < f->cfi_nsaved; k++) {
                u8(out, CFA_offset | (unsigned)f->cfi_reg[k]);
                uleb(out, (unsigned long)(-f->cfi_off[k] / 8));
            }
        }
        close_entry(out, fde);
    }
    /* no zero terminator: the linked image's .eh_frame ends with one */
}

void eh_free(struct eh_out *out)
{
    free(out->frame);
    free(out->relocs);
    memset(out, 0, sizeof *out);
}
