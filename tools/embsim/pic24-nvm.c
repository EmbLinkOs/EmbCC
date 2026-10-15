/* pic24-nvm.c -- a PIC24FJ's flash controller (NVMCON, NVMADR, NVMADRU,
 * NVMKEY: the EDC's NVM), programming the part's own program memory
 * (RTSP), as the data sheet's section 6 has it.
 *
 *   - The write latches: 128 instruction words that TBLWTL and TBLWTH
 *     write with TBLPAG 0xFA (0xFA0000-0xFA00FE), and the table reads
 *     read there; out of reset they hold 0xFFFFFF (the part's are
 *     unknown);
 *   - NVMKEY written 0x55 then 0xAA unlocks the next instruction: NVMCON.WR
 *     set then, with WREN set, runs NVMOP's operation at NVMADRU:NVMADR --
 *     0001 two words from the latches, 0010 a row (128 words, the address
 *     on a row's boundary), 0011 erases a page (1024 words), 1110 erases
 *     the user memory -- the CPU stalled for its time (20 us a double
 *     word, 1.5 ms a row, 20 ms an erase, the data sheet's typical), WR
 *     cleared and the NVM interrupt's flag set after it. A program
 *     operation clears bits only, as flash does; an erase makes the words
 *     0xFFFFFF;
 *   - WR set without the unlock, or without WREN, or with an NVMOP the
 *     part has not, sets WRERR instead.
 * The unlock is the core's (pic24_unlocked): the RTCC's WRLOCK takes the
 * same one. Not modelled: the dual partition modes (BOOTSWP, NVMOP 0100
 * and 1000), the code protection. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NVM_WR      (1u << 15)
#define NVM_WREN    (1u << 14)
#define NVM_WRERR   (1u << 13)

struct nvm {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *con, *adr, *adru, *key;
    u32 latch[128];
    int irq;
    struct pic24_nvm core;
};

static u32 latch_read(void *ctx, u32 addr)
{
    struct nvm *m = ctx;
    return m->latch[(addr >> 1) & 127];
}

static void latch_write(void *ctx, u32 addr, int high, int n, u16 v)
{
    struct nvm *m = ctx;
    u32 *w = &m->latch[(addr >> 1) & 127];
    if (high) {
        if (n == 2 || !(addr & 1))  /* the high byte; the phantom byte is none */
            *w = (*w & 0x00ffff) | (u32)(v & 0xff) << 16;
    } else if (n == 2) {
        *w = (*w & 0xff0000) | v;
    } else if (addr & 1) {
        *w = (*w & 0xff00ff) | (u32)(v & 0xff) << 8;
    } else {
        *w = (*w & 0xffff00) | (v & 0xff);
    }
}

/* the program word at a (a program address) on the bus */
static u8 *word_at(struct nvm *m, u32 a)
{
    struct region *r = bus_region(&m->s->bus, 2 * a, 4);
    return r && r->kind == MEM_FLASH ? r->mem + (2 * a - r->base) : 0;
}

static void program(struct nvm *m, u32 a, int words)
{
    for (int k = 0; k < words; k++) {
        u8 *b = word_at(m, a + 2 * (u32)k);
        u32 v = m->latch[k & 127];
        if (!b)
            continue;
        b[0] &= (u8)v;
        b[1] &= (u8)(v >> 8);
        b[2] &= (u8)(v >> 16);
    }
}

static void erase(struct nvm *m, u32 a, u32 words)
{
    for (u32 k = 0; k < words; k++) {
        u8 *b = word_at(m, a + 2 * k);
        if (b) {
            b[0] = b[1] = b[2] = 0xff;
            b[3] = 0;
        }
    }
}

/* the CPU stalled for us microseconds */
static void stall(struct nvm *m, u32 us)
{
    struct sim *s = m->s;
    u32 n = (u32)((u64)pic24_fcy(s) * us / 1000000u);
    s->cycles += n;
    if (s->counting)
        sim_advance(s, n);
}

static void start(struct nvm *m)
{
    u32 con = (u32)m->con->value;
    u32 a = (u32)(m->adru ? (m->adru->value & 0xff) << 16 : 0) | (u32)(m->adr->value & 0xfffe);
    if (!pic24_unlocked(m->s) || !(con & NVM_WREN)) {
        rf_hw(m->con, (con & ~NVM_WR) | NVM_WRERR);
        return;
    }
    switch (con & 15) {
    case 0x1: program(m, a & ~3u, 2); stall(m, 20); break;
    case 0x2: program(m, a & ~0xffu, 128); stall(m, 1500); break;
    case 0x3: erase(m, a & ~0x7ffu, 1024); stall(m, 20000); break;
    case 0xe: erase(m, 0, ((struct pic24_state *)m->s->cpu)->prog_end / 2); stall(m, 20000); break;
    default:
        rf_hw(m->con, (con & ~NVM_WR) | NVM_WRERR);
        return;
    }
    rf_hw(m->con, con & ~(NVM_WR | NVM_WRERR));
    if (m->irq >= 0)
        pic24_irq(m->s, m->irq);
}

static u32 nvm_read(void *ctx, u32 off, int n)
{
    struct nvm *m = ctx;
    return rf_read(m->p, off, n);
}

static void nvm_write(void *ctx, u32 off, int n, u32 v)
{
    struct nvm *m = ctx;
    if (off == rf_off(m->key)) {
        pic24_nvmkey(m->s, (u16)v);
        return;                     /* write-only: it reads 0 */
    }
    u64 was = m->con->value;
    rf_write(m->p, off, n, v);
    if (off == rf_off(m->con) && (m->con->value & NVM_WR) && !(was & NVM_WR))
        start(m);
}

static void nvm_reset(void *ctx)
{
    struct nvm *m = ctx;
    for (int k = 0; k < 128; k++)
        m->latch[k] = 0xffffff;
}

const struct dev_ops pic24_nvm_ops = {
    "pic24-nvm", nvm_read, nvm_write, nvm_reset, 0, 0,
};

void *pic24_nvm_create(struct sim *s, struct rf_periph *p)
{
    struct nvm *m = calloc(1, sizeof *m);
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    m->con = rf_reg(p, "NVMCON");
    m->adr = rf_reg(p, "NVMADR");
    if (!m->adr)
        m->adr = rf_reg(p, "NVMADRL");
    m->adru = rf_reg(p, "NVMADRU");
    if (!m->adru)
        m->adru = rf_reg(p, "NVMADRH");
    m->key = rf_reg(p, "NVMKEY");
    if (!m->con || !m->adr || !m->key) {
        free(m);
        return 0;
    }
    m->irq = pic24_irq_named(s, "NVM");
    m->core.latch_read = latch_read;
    m->core.latch_write = latch_write;
    m->core.ctx = m;
    pic24_nvm_attach(s, &m->core);
    nvm_reset(m);
    return m;
}
