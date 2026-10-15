/* rp2040-ssi.c -- the RP2040's SSI (a Synopsys DW_apb_ssi, its datasheet's
 * 4.10, at 0x18000000) and the Pico's flash behind it, a Winbond W25Q16JV
 * (2 MiB): what boot2 and a program that talks to the flash itself see.
 *
 * The flash's contents are the board's flash region (0x10000000): EmbSim
 * reads the XIP window straight from it, whatever the SSI is set to (the
 * cache, its flush and its counters are not modelled). Through the SSI:
 *   - while SSIENR is set, a frame written to DR0 is sent; each transfer
 *     takes no time, so SR says the transmit FIFO is empty and the SSI not
 *     busy (TFE, TFNF), and RFNE while a received frame waits;
 *   - TMOD 0 (transmit and receive): each frame sent brings one back, the
 *     flash's answer to the command so far; TMOD 3 (EEPROM read): the
 *     frames sent are the instruction and address (in the dual and quad
 *     formats, as SPI_CTRLR0's INST_L and ADDR_L say), and CTRLR1.NDF + 1
 *     frames come back;
 *   - the chip select: a command lasts until the program reads SR or DR0
 *     after writing (it has waited for the transfer, and the SSI lets the
 *     select go when its FIFO empties) or disables the SSI -- unless
 *     GPIO_QSPI_SS_CTRL's OUTOVER holds the select low, when it lasts
 *     until OUTOVER lets it go.
 * The flash's commands: READ (03h), FAST READ (0Bh), the quad I/O fast
 * read (EBh) and its continuous mode (M7-4 = 1010b), READ STATUS 1, 2 and
 * 3 (05h, 35h, 15h), WRITE STATUS 1 (01h, and status 2 after it) and 2
 * (31h), WRITE ENABLE (06h) and DISABLE (04h), JEDEC ID (9Fh: EF 40 15),
 * MANUFACTURER/DEVICE ID (90h: EF 14), RELEASE POWER-DOWN (ABh: 14), the
 * UNIQUE ID (4Bh), PAGE PROGRAM (02h: bits only clear, within the page),
 * SECTOR, 32K and 64K BLOCK and CHIP ERASE (20h, 52h, D8h, C7h/60h) --
 * the last two kinds need WRITE ENABLE, and finish at once (BUSY never
 * reads 1). Status register 2 powers up with QE 0, so boot2 sets it.
 * EmbSim's unique ID is 0x456d6253696d0001 ("EmbSim", 1). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "rp2040.h"
#include "svd-map.h"

#define FLASH_BASE 0x10000000u

struct ssi {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *ctrlr0, *ctrlr1, *ssienr, *txflr, *rxflr, *sr, *dr, *spi_ctrlr0;
    u64 m_tmod, m_dfs32, m_frf, m_ndf, m_inst_l, m_addr_l;
    u32 rx[16];
    int nrx, rxh;
    /* the command in progress */
    u8 cmd[264];
    int ncmd;
    int selected;
    u8 sr1, sr2, sr3;
    int continuous;                 /* the EBh continuous read mode */
};

static const u8 unique_id[8] = { 0x45, 0x6d, 0x62, 0x53, 0x69, 0x6d, 0x00, 0x01 };

static u32 fld(const struct rf_reg *r, u64 mask)
{
    int sh = 0;
    if (!mask)
        return 0;
    while (!(mask >> sh & 1))
        sh++;
    return (u32)((r->value & mask) >> sh);
}

static struct region *flash(struct ssi *x)
{
    return bus_region(&x->s->bus, FLASH_BASE, 1);
}

static u8 flash_byte(struct ssi *x, u32 a)
{
    struct region *f = flash(x);
    return f ? f->mem[a % f->size] : 0xff;
}

static u32 addr24(const u8 *c)
{
    return (u32)c[1] << 16 | (u32)c[2] << 8 | c[3];
}

/* the command's bytes so far are done with: what it does at its end */
static void finish(struct ssi *x)
{
    struct region *f = flash(x);
    const u8 *c = x->cmd;
    int n = x->ncmd;
    x->ncmd = 0;
    if (!n)
        return;
    switch (c[0]) {
    case 0x06: x->sr1 |= 2; return;
    case 0x04: x->sr1 &= ~2; return;
    case 0x01:
        if (x->sr1 & 2 && n >= 2) {
            x->sr1 = (u8)((c[1] & 0xfc) | (x->sr1 & 3));
            if (n >= 3)
                x->sr2 = c[2];
            x->sr1 &= ~2;
        }
        return;
    case 0x31:
        if (x->sr1 & 2 && n >= 2) {
            x->sr2 = c[1];
            x->sr1 &= ~2;
        }
        return;
    case 0x02:
        if (x->sr1 & 2 && n >= 5 && f) {
            u32 a = addr24(c);
            for (int i = 4; i < n; i++) {
                u32 at = (a & ~0xffu) | ((a + (u32)(i - 4)) & 0xff);
                f->mem[at % f->size] &= c[i];
            }
            x->sr1 &= ~2;
        }
        return;
    case 0x20: case 0x52: case 0xd8: case 0xc7: case 0x60:
        if (x->sr1 & 2 && f && (n >= 4 || c[0] == 0xc7 || c[0] == 0x60)) {
            u32 sz = c[0] == 0x20 ? 0x1000 : c[0] == 0x52 ? 0x8000
                   : c[0] == 0xd8 ? 0x10000 : f->size;
            u32 a = (c[0] == 0xc7 || c[0] == 0x60) ? 0 : addr24(c) & ~(sz - 1);
            for (u32 i = 0; i < sz && a + i < f->size; i++)
                f->mem[a + i] = 0xff;
            x->sr1 &= ~2;
        }
        return;
    case 0x66: case 0x99:
        x->sr1 &= 0xfc;
        x->continuous = 0;
        return;
    }
}

/* the byte the flash sends back as the command's byte k comes in */
static u8 answer(struct ssi *x, int k)
{
    const u8 *c = x->cmd;
    switch (c[0]) {
    case 0x05: return x->sr1;
    case 0x35: return x->sr2;
    case 0x15: return x->sr3;
    case 0x9f: return k == 1 ? 0xef : k == 2 ? 0x40 : k == 3 ? 0x15 : 0xff;
    case 0x90: return k >= 4 ? ((k - 4) & 1 ? 0x14 : 0xef) : 0xff;
    case 0xab: return k >= 4 ? 0x14 : 0xff;
    case 0x4b: return k >= 5 && k < 13 ? unique_id[k - 5] : 0xff;
    case 0x03: return k >= 4 ? flash_byte(x, addr24(c) + (u32)(k - 4)) : 0xff;
    case 0x0b: return k >= 5 ? flash_byte(x, addr24(c) + (u32)(k - 5)) : 0xff;
    }
    return 0xff;
}

static void rx_push(struct ssi *x, u32 v)
{
    if (x->nrx < 16)
        x->rx[(x->rxh + x->nrx++) % 16] = v;
}

static void cmd_byte(struct ssi *x, u8 b)
{
    if (x->ncmd < (int)sizeof x->cmd)
        x->cmd[x->ncmd++] = b;
}

/* a frame written to DR0 */
static void frame(struct ssi *x, u32 v)
{
    int tmod = (int)fld(x->ctrlr0, x->m_tmod), bits = (int)fld(x->ctrlr0, x->m_dfs32) + 1;
    int frf = (int)fld(x->ctrlr0, x->m_frf);
    int bytes = (bits + 7) / 8;
    x->selected = 1;
    if (tmod == 3) {
        /* EEPROM read: the instruction, the address, then NDF + 1 frames */
        int inst = frf && !fld(x->spi_ctrlr0, x->m_inst_l) ? 0 : 1;
        if (frf) {
            if (inst && x->ncmd == 0) {
                cmd_byte(x, (u8)v);
                return;
            }
            /* the address and mode word, ADDR_L x 4 bits */
            int abits = (int)fld(x->spi_ctrlr0, x->m_addr_l) * 4;
            u32 a = abits >= 32 ? v >> 8 : v;
            if (abits >= 32 && (v & 0xf0) == 0xa0)
                x->continuous = 1;
            u32 n = fld(x->ctrlr1, x->m_ndf) + 1;
            for (u32 i = 0; i < n; i++) {
                u32 w = 0;
                for (int b = 0; b < bytes; b++)
                    w = w << 8 | flash_byte(x, a + i * (u32)bytes + (u32)b);
                rx_push(x, w);
            }
            x->ncmd = 0;
            return;
        }
        cmd_byte(x, (u8)v);
        if (x->ncmd >= 4) {
            u32 n = fld(x->ctrlr1, x->m_ndf) + 1, a = addr24(x->cmd);
            for (u32 i = 0; i < n; i++)
                rx_push(x, flash_byte(x, a + i));
            x->ncmd = 0;
        }
        return;
    }
    /* standard SPI, byte frames: the command so far, and its answer */
    for (int b = bytes - 1; b >= 0; b--) {
        cmd_byte(x, (u8)(v >> (8 * b)));
        if (tmod != 1)              /* transmit only: nothing comes back */
            rx_push(x, answer(x, x->ncmd - 1));
    }
}

/* the select goes high, unless IO_QSPI holds it low */
static void deselect(struct ssi *x)
{
    struct rf_periph *q = x->s->svd ? rf_periph(x->s->svd, "IO_QSPI") : 0;
    struct rf_reg *ss = q ? rf_reg(q, "GPIO_QSPI_SS_CTRL") : 0;
    if (ss && fld(ss, rf_mask(ss, "OUTOVER")) == 2)
        return;
    if (x->selected) {
        finish(x);
        x->selected = 0;
    }
}

static void status(struct ssi *x)
{
    /* TFE and TFNF: every frame has gone; RFNE, RFF */
    u64 v = 6 | (x->nrx ? 8 : 0) | (x->nrx == 16 ? 16 : 0);
    x->sr->value = v;
    x->txflr->value = 0;
    x->rxflr->value = (u64)x->nrx;
}

static u32 ssi_read(void *ctx, u32 off, int n)
{
    struct ssi *x = ctx;
    if (!x->s->bus.debug && (off == rf_off(x->sr) || off == rf_off(x->dr)))
        deselect(x);
    status(x);
    if (off == rf_off(x->dr)) {
        u32 v = x->nrx ? x->rx[x->rxh] : 0;
        if (x->nrx && !x->s->bus.debug) {
            x->rxh = (x->rxh + 1) % 16;
            x->nrx--;
            status(x);
        }
        return v;
    }
    return rf_read(x->p, off, n);
}

static void ssi_write(void *ctx, u32 off, int n, u32 v)
{
    struct ssi *x = ctx;
    rf_write(x->p, off, n, v);
    if (off == rf_off(x->ssienr) && !(v & 1)) {
        deselect(x);
        x->nrx = 0;                 /* disabling clears the FIFOs */
    }
    if (off == rf_off(x->dr) && (x->ssienr->value & 1))
        frame(x, v);
    status(x);
}

static void ssi_reset(void *ctx)
{
    struct ssi *x = ctx;
    x->nrx = x->rxh = x->ncmd = x->selected = 0;
    status(x);
}

const struct dev_ops rp2040_ssi_ops = {
    "rp2040-ssi", ssi_read, ssi_write, ssi_reset, 0, 0,
};

/* the chip select held low by IO_QSPI was let go */
void rp2040_ssi_select(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "SSI") : 0;
    if (p && p->ops == &rp2040_ssi_ops)
        deselect(p->ctx);
}

void *rp2040_ssi_create(struct sim *s, struct rf_periph *p)
{
    struct ssi *x = calloc(1, sizeof *x);
    if (!x)
        die("out of memory");
    x->s = s;
    x->p = p;
    if (!(x->ctrlr0 = rf_reg(p, "CTRLR0")) || !(x->ctrlr1 = rf_reg(p, "CTRLR1")) ||
        !(x->ssienr = rf_reg(p, "SSIENR")) || !(x->txflr = rf_reg(p, "TXFLR")) ||
        !(x->rxflr = rf_reg(p, "RXFLR")) || !(x->sr = rf_reg(p, "SR")) ||
        !(x->dr = rf_reg(p, "DR0")) || !(x->spi_ctrlr0 = rf_reg(p, "SPI_CTRLR0"))) {
        free(x);
        return 0;
    }
    x->m_tmod = rf_mask(x->ctrlr0, "TMOD");
    x->m_dfs32 = rf_mask(x->ctrlr0, "DFS_32");
    x->m_frf = rf_mask(x->ctrlr0, "SPI_FRF");
    x->m_ndf = rf_mask(x->ctrlr1, "NDF");
    x->m_inst_l = rf_mask(x->spi_ctrlr0, "INST_L");
    x->m_addr_l = rf_mask(x->spi_ctrlr0, "ADDR_L");
    rf_set_reset(x->sr, 6);         /* TFE, TFNF: the SVD's 0 would wait for ever */
    return x;
}
