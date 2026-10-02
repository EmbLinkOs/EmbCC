#include "code.h"

#include "../driver/util.h"

void code_mark_data(struct code *c, int start, int end)
{
    if (end <= start)
        return;
    if (c->ndrange + 2 > c->capdrange) {
        c->capdrange = c->capdrange ? c->capdrange * 2 : 16;
        c->drange = xrealloc(c->drange, (size_t)c->capdrange * sizeof *c->drange);
    }
    c->drange[c->ndrange++] = start;
    c->drange[c->ndrange++] = end;
}

void code_byte(struct code *c, int b)
{
    if (c->len == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->p = xrealloc(c->p, (size_t)c->cap);
    }
    c->p[c->len++] = (unsigned char)b;
}

void code_u32(struct code *c, unsigned long v)
{
    code_byte(c, (int)(v & 0xff));
    code_byte(c, (int)((v >> 8) & 0xff));
    code_byte(c, (int)((v >> 16) & 0xff));
    code_byte(c, (int)((v >> 24) & 0xff));
}

/* Two bytes, little-endian: a RISC-V compressed instruction, and the
 * halfword a Thumb one is built from. */
void code_u16(struct code *c, unsigned v)
{
    code_byte(c, (int)(v & 0xff));
    code_byte(c, (int)((v >> 8) & 0xff));
}

void code_patch32(struct code *c, int off, unsigned long v)
{
    c->p[off] = (unsigned char)(v & 0xff);
    c->p[off + 1] = (unsigned char)((v >> 8) & 0xff);
    c->p[off + 2] = (unsigned char)((v >> 16) & 0xff);
    c->p[off + 3] = (unsigned char)((v >> 24) & 0xff);
}

void code_align(struct code *c, int align, int fill)
{
    while (c->len % align)
        code_byte(c, fill);
}
