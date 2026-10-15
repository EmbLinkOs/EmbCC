/* mips.c -- the MIPS32 core: Release 2, little-endian, as a PIC32MZ's
 * microAptiv-class core runs it (`pic32mz`), and as QEMU's malta runs a
 * 24Kc (`24kc`).
 *
 * The instruction set is the whole MIPS32r2 integer set: the branches
 * and jumps with their delay slots (the likely forms skipping theirs
 * when not taken), HI and LO's multiplies and divides, mul, madd and
 * msub, clz and clo, ext and ins, seb, seh and wsbh, rotr, the traps,
 * ll and sc, lwl, lwr, swl and swr, rdhwr; coprocessor 0's mfc0 and mtc0,
 * di and ei, rdpgpr and wrpgpr, eret, deret, wait and the TLB's
 * instructions; cache, pref, sync and synci as no-ops. The DSP ASE
 * (Revision 2, the `pic32mz` core's, with Status.MX): HI and LO of
 * $ac1-$ac3 for mfhi, mflo, mthi, mtlo and the multiplies, rddsp and
 * wrdsp, lwx, lhx and lbux, bposge32 here, and the rest in mips-dsp.c.
 * A microMIPS instruction ends the run as a lockup that names it, rather
 * than doing what no part does; an encoding no MIPS32r2 core has is a
 * Reserved Instruction exception, as on the part.
 *
 * Exceptions are the privileged architecture's: EPC and Cause.BD for an
 * instruction in a delay slot, Status.EXL and ERL, the boot vectors
 * while Status.BEV is set and EBase's after, the TLB refill vector, and
 * BadVAddr. Interrupts come in compatibility mode (Cause.IP and
 * Status.IM; the timer on IP7), vectored mode (IntCtl.VS), or -- on a
 * PIC32 -- EIC mode, from the EVIC (mips.h's struct mips_eic): the
 * requested level against Status.IPL, the handler at EBase plus the
 * controller's offset (a PIC32MZ's OFFn), and the shadow register set
 * it names. Shadow sets follow SRSCtl: an exception or interrupt saves
 * CSS in PSS and switches, eret switches back, and rdpgpr and wrpgpr
 * reach the previous set. Count counts every other cycle; Compare's
 * match sets Cause.TI, the core timer's interrupt.
 *
 * sdbbp enters debug mode, which needs a probe: with none it ends the
 * run as a lockup, at the breakpoint, with what the program was doing --
 * which is what XC32's and EmbCC's startups use it for (a default
 * handler, an exception nobody handles).
 *
 * Memory: kseg0 and kseg1 reach the bus's low 512 MiB unmapped; kuseg,
 * kseg2 and kseg3 go through the TLB (kuseg unmapped while Status.ERL
 * is set, as after reset). An access to nothing is a bus error
 * exception. Loads and stores must be aligned, but for lwl, lwr, swl and
 * swr. sc succeeds when ll's address still holds the value ll read
 * (QEMU's rule), and eret clears the link. */
#include <stdlib.h>
#include <string.h>

#include "../bench/cost.h"
#include "devices.h"
#include "mips.h"

typedef int8_t s8;
typedef int16_t s16;

struct mips_state *ms;

/* ---- exceptions ------------------------------------------------------ */

static void raise_exc(u32 code)
{
    if (ms->exc)
        return;
    ms->exc = 1;
    ms->exc_code = code;
    ms->exc_bad_set = 0;
    ms->exc_refill = 0;
    ms->exc_ce = 0;
}

static void raise_addr(u32 code, u32 va)
{
    if (ms->exc)
        return;
    raise_exc(code);
    ms->exc_bad = va;
    ms->exc_bad_set = 1;
}

static void raise_cpu(u32 unit)
{
    if (ms->exc)
        return;
    raise_exc(EXC_CPU);
    ms->exc_ce = unit;
}

static void reserved(void)
{
    raise_exc(EXC_RI);
}

/* an instruction the part has and EmbSim does not model */
static void unmodelled(const char *what)
{
    if (ms->exc)
        return;
    raise_exc(EXC_RI);
    sim_end(ms->sim, END_LOCKUP, "%s (0x%08x at 0x%08x) is not modelled",
            what, ms->insn, ms->pc);
}

static int kernel(void)
{
    return !(ms->status & ST_UM) || (ms->status & (ST_EXL | ST_ERL)) ||
           (ms->debug & DBG_DM);
}

/* the register set an exception or interrupt switches to: Release 2's
 * shadow sets, when the core has them and the boot vectors are not in
 * use */
static void switch_set(int css)
{
    u32 hss = (ms->srsctl >> 26) & 15;
    if (!hss || (ms->status & ST_BEV))
        return;
    if ((u32)css > hss)
        css = 0;
    u32 cur = ms->srsctl & 15;
    ms->srsctl = (ms->srsctl & ~(15u << 6 | 15u)) | cur << 6 | (u32)css;
    ms->r = ms->gpr[css];
}

/* Exception entry: EPC (and Cause.BD) for the instruction at pc unless
 * one is in progress (Status.EXL), Cause's code, the vector. `intoff`
 * is an interrupt's offset from the vector base, or 0 for an exception. */
static void enter(u32 code, u32 intoff, int newset)
{
    struct sim *s = ms->sim;
    u32 st = ms->status, off;
    u32 ret = ms->slot ? ms->pc - 4 : ms->pc;
    ms->exc_regs = ms->r;
    if (!(st & ST_EXL)) {
        ms->epc = ret;
        ms->cause = ms->slot ? ms->cause | CA_BD : ms->cause & ~CA_BD;
        off = code == EXC_INT ? intoff : ms->exc_refill ? 0 : 0x180;
        switch_set(newset);
    } else {
        off = 0x180;
    }
    /* CE is the unit of a Coprocessor Unusable exception, and as it was
     * for the others (the architecture leaves it unpredictable; QEMU's
     * keeps it) */
    ms->cause = (ms->cause & ~0x7cu) | code << 2;
    if (code == EXC_CPU)
        ms->cause = (ms->cause & ~(3u << 28)) | ms->exc_ce << 28;
    if (ms->exc_bad_set)
        ms->badvaddr = ms->exc_bad;
    ms->status |= ST_EXL;
    u32 base = (st & ST_BEV) ? 0xbfc00200u : (ms->ebase & 0xfffff000u);
    ms->pc = base + off;
    ms->npc = ms->pc + 4;
    ms->slot = 0;
    if (s->an)
        an_exc_entry(s, code, ret);
}

/* ---- the core timer and the interrupt lines ------------------------------ */

static u32 count_now(void)
{
    if (ms->cause & CA_DC)
        return ms->count_base;
    return ms->count_base + (u32)((ms->sim->cycles - ms->count_at) >> 1);
}

/* when Count next equals Compare: it counts every other cycle, and a
 * Compare equal to Count now is a whole turn away */
static void timer_plan(void)
{
    struct sim *s = ms->sim;
    s->bus.touched = 1;
    if (ms->cause & CA_DC) {
        ms->ti_at = ~0ull;
        return;
    }
    u64 k = (s->cycles - ms->count_at) >> 1;
    u64 d = (u32)(ms->compare - count_now());
    if (!d)
        d = 1ull << 32;
    ms->ti_at = ms->count_at + 2 * (k + d);
}

/* what the core requests of an EIC: the timer and the two software
 * interrupts, told when they change */
static void core_lines(void)
{
    u32 l = ((ms->cause & CA_TI) ? 1u : 0) | ((ms->cause >> 7) & 6u);
    if (ms->eic && l != ms->core_lines) {
        ms->core_lines = l;
        ms->eic->core_irq(ms->eic->ctx, l & 1, (l >> 1) & 1, (l >> 2) & 1);
    }
    ms->core_lines = l;
}

static void timer_fire(void)
{
    ms->cause |= CA_TI;
    ms->ti_at += 2ull << 32;
    core_lines();
    ms->sim->bus.touched = 1;
}

/* the IP bits of compatibility and vectored modes: IP1-IP0 written by
 * the program, IP2-IP7 the hardware's lines, the timer on IntCtl.IPTI */
static u32 ip_bits(void)
{
    u32 ip = (ms->cause >> 8) & 3;
    ip |= (ms->ip_hw & 0x3f) << 2;
    if (ms->cause & CA_TI)
        ip |= 1u << ((ms->intctl >> 29) & 7);
    return ip;
}

static u32 ipl(void)
{
    return ((ms->status >> 10) & 0x7f) | ((ms->status >> 11) & 0x80);
}

static int interrupts_on(void)
{
    return (ms->status & ST_IE) && !(ms->status & (ST_EXL | ST_ERL)) &&
           !(ms->debug & DBG_DM);
}

/* an interrupt the core would take now */
static int interrupt_ready(void)
{
    if (!interrupts_on())
        return 0;
    if (ms->eic)
        return (u32)ms->eic->ripl > ipl();
    return (ip_bits() & (ms->status >> 8) & 0xff) != 0;
}

static void take_interrupt(void)
{
    u32 off = 0x200, vs = (ms->intctl >> 5) & 31;
    int set = (int)((ms->srsctl >> 12) & 15);
    if (!(ms->cause & CA_IV)) {
        off = 0x180;
    } else if (ms->eic) {
        ms->cause = (ms->cause & ~(0xffu << 10)) | (u32)ms->eic->ripl << 10;
        if (!(ms->status & ST_BEV) && vs)
            off = ms->eic->offset;
        set = ms->eic->srs;
        ms->srsctl = (ms->srsctl & ~(15u << 18)) | (u32)(set & 15) << 18;
    } else if (vs && !(ms->status & ST_BEV)) {
        /* vectored: the highest IP pending and enabled */
        u32 p = ip_bits() & (ms->status >> 8) & 0xff;
        int v = 7;
        while (v > 0 && !(p >> v & 1))
            v--;
        off = 0x200 + (u32)v * (vs << 5);
        set = (int)((ms->srsmap >> (4 * v)) & 15);
    }
    if (ms->eic && ms->eic->taken)
        ms->eic->taken(ms->eic->ctx);
    ms->exc_bad_set = 0;
    ms->exc_refill = 0;
    enter(EXC_INT, off, set);
}

void mips_eic_attach(struct sim *s, struct mips_eic *e)
{
    struct mips_state *k = (struct mips_state *)s->cpu;
    if (!mips_is(s->cpu))
        die("an interrupt controller for a MIPS core on a board without one");
    k->eic = e;
}

void mips_eic_changed(struct sim *s)
{
    s->bus.touched = 1;
}

/* the core timer, as a device: when its interrupt comes, for a wait */
static int ctimer_next(void *ctx, u32 *cycles)
{
    struct mips_state *k = ctx;
    struct sim *s = k->sim;
    if ((k->cause & (CA_DC | CA_TI)) || k->ti_at == ~0ull)
        return 0;
    if (k->eic ? !(k->eic->timer_on && k->eic->timer_on(k->eic->ctx))
               : !((k->status >> 8) & (1u << ((k->intctl >> 29) & 7))))
        return 0;
    u64 d = k->ti_at > s->cycles ? k->ti_at - s->cycles : 0;
    *cycles = d > 0xffffffffu ? 0xffffffffu : (u32)d;
    return 1;
}

static const struct dev_ops ctimer_ops = { "mips-count", 0, 0, 0, 0, ctimer_next };

/* ---- memory ---------------------------------------------------------- */

u32 mips_phys(u32 va)
{
    return va >= 0x80000000u && va < 0xc0000000u ? va & 0x1fffffffu : va;
}

/* the TLB entry mapping va, or -1 */
static int tlb_find(u32 va, u32 asid)
{
    for (int i = 0; i < MIPS_TLB; i++) {
        const struct mips_tlbe *t = &ms->tlb[i];
        u32 m = ~(t->mask | 0x1fffu);
        if (!((va ^ t->hi) & m) && (t->g || (t->hi & 0xff) == asid))
            return i;
    }
    return -1;
}

/* 0 and the physical address, or -1 with the exception raised */
static int tlb_xlate(u32 va, int acc, u32 *pa)
{
    u32 code = acc == 1 ? EXC_TLBS : EXC_TLBL;
    int i = tlb_find(va, ms->entryhi & 0xff);
    if (i < 0) {
        raise_addr(code, va);
        ms->exc_refill = !(ms->status & ST_EXL);
    } else {
        const struct mips_tlbe *t = &ms->tlb[i];
        u32 half = ((t->mask | 0x1fffu) + 1) >> 1;
        u32 lo = (va & half) ? t->lo1 : t->lo0;
        if (!(lo & 2)) {
            raise_addr(code, va);
        } else if (acc == 1 && !(lo & 4)) {
            raise_addr(EXC_MOD, va);
        } else {
            *pa = ((lo >> 6) << 12 & ~(half - 1)) | (va & (half - 1));
            return 0;
        }
    }
    ms->context = (ms->context & 0xff800000u) | (va >> 9 & 0x7ffff0u);
    ms->entryhi = (va & 0xffffe000u) | (ms->entryhi & 0xff);
    return -1;
}

/* the physical address of an access (acc: 0 a load, 1 a store, 2 a
 * fetch), or -1 with its exception raised */
static int xlate(u32 va, int acc, u32 *pa)
{
    if (va >= 0x80000000u) {
        if (!kernel()) {
            raise_addr(acc == 1 ? EXC_ADES : EXC_ADEL, va);
            return -1;
        }
        if (va < 0xc0000000u) {
            *pa = va & 0x1fffffffu;
            return 0;
        }
        return tlb_xlate(va, acc, pa);
    }
    if (ms->status & ST_ERL) {
        *pa = va;
        return 0;
    }
    return tlb_xlate(va, acc, pa);
}

static int watched(u32 va, int n, int write)
{
    struct bus *b = &ms->sim->bus;
    if (b->nwatch && bus_watch_check(b, va, n, write)) {
        ms->watch_stop = 1;
        ms->exc = 1;
        return 1;
    }
    return 0;
}

/* n aligned bytes at physical pa: 0, or -1 for nothing there */
static int pread(u32 pa, int n, u32 *v)
{
    struct region *r = bus_region(&ms->sim->bus, pa, (u32)n);
    if (r) {
        *v = mem_rd_le(r->mem + (pa - r->base), n);
        return 0;
    }
    return bus_read(&ms->sim->bus, pa, n, v);
}

/* n bytes, any of 1 to 4, from pa up (swl and swr write 3, and swr at
 * any address): a device gets its aligned accesses, the rest a byte at
 * a time */
static int pwrite(u32 pa, int n, u32 v)
{
    struct bus *b = &ms->sim->bus;
    struct region *r = bus_region(b, pa, (u32)n);
    if (r) {
        if (!r->rom)
            for (int i = 0; i < n; i++)
                r->mem[pa - r->base + (u32)i] = (u8)(v >> (8 * i));
        return 0;
    }
    if (n == 1 || (n == 2 && !(pa & 1)) || (n == 4 && !(pa & 3)))
        return bus_write(b, pa, n, n == 4 ? v : v & ((1u << (8 * n)) - 1));
    for (int i = 0; i < n; i++)
        if (bus_write(b, pa + (u32)i, 1, (v >> (8 * i)) & 0xff))
            return -1;
    return 0;
}

static u32 load(u32 va, int n)
{
    u32 pa, v;
    if (ms->exc)
        return 0;
    if (va & (u32)(n - 1)) {
        raise_addr(EXC_ADEL, va);
        return 0;
    }
    if (xlate(va, 0, &pa) || watched(va, n, 0))
        return 0;
    if (pread(pa, n, &v)) {
        raise_exc(EXC_DBE);
        return 0;
    }
    return v;
}

static void store(u32 va, int n, u32 v)
{
    u32 pa;
    if (ms->exc)
        return;
    if (va & (u32)(n - 1)) {
        raise_addr(EXC_ADES, va);
        return;
    }
    if (xlate(va, 1, &pa) || watched(va, n, 1))
        return;
    if (pwrite(pa, n, v))
        raise_exc(EXC_DBE);
}

u32 mips_load(u32 va, int n)
{
    return load(va, n);
}

void mips_store(u32 va, int n, u32 v)
{
    store(va, n, v);
}

void mips_raise(u32 code)
{
    raise_exc(code);
}

/* ---- the registers ----------------------------------------------------- */

static inline u32 R(u32 n)
{
    return ms->r[n];
}

static inline void W(u32 n, u32 v)
{
    if (n)
        ms->r[n] = v;
}

/* ---- coprocessor 0 ------------------------------------------------------- */

static u32 status_mask(void)
{
    u32 m = ST_CU0 | (1u << 27) | ST_BEV | ST_SR | ST_NMI | ST_UM | ST_ERL |
            ST_EXL | ST_IE | 0x0005ff00u;
    if (ms->fpu)
        m |= ST_CU1 | (1u << 26);
    if (ms->dsp)
        m |= ST_MX;
    return m;
}

static u32 cp0_read(u32 rd, u32 sel)
{
    switch (rd << 3 | sel) {
    case 0 << 3: return ms->index;
    case 1 << 3: {                      /* Random: from Wired to the last */
        u32 w = ms->wired & 15;
        return w >= MIPS_TLB - 1 ? MIPS_TLB - 1
               : w + (u32)(ms->sim->insns % (MIPS_TLB - w));
    }
    case 2 << 3: return ms->entrylo0;
    case 3 << 3: return ms->entrylo1;
    case 4 << 3: return ms->context;
    case 4 << 3 | 2: return ms->userlocal;
    case 5 << 3: return ms->pagemask;
    case 6 << 3: return ms->wired;
    case 7 << 3: return ms->hwrena;
    case 8 << 3: return ms->badvaddr;
    case 9 << 3: return count_now();
    case 10 << 3: return ms->entryhi;
    case 11 << 3: return ms->compare;
    case 12 << 3: return ms->status;
    case 12 << 3 | 1: return ms->intctl;
    case 12 << 3 | 2: return ms->srsctl;
    case 12 << 3 | 3: return ms->srsmap;
    case 13 << 3:
        if (ms->eic)
            return ms->cause;
        return (ms->cause & ~0xfc00u) | (ip_bits() & 0xfc) << 8;
    case 14 << 3: return ms->epc;
    case 15 << 3: return ms->prid;
    case 15 << 3 | 1: return ms->ebase;
    case 16 << 3: return ms->config0;
    case 16 << 3 | 1: return ms->config1;
    case 16 << 3 | 2: return ms->config2;
    case 16 << 3 | 3: return ms->config3;
    case 16 << 3 | 5: return ms->config5;
    case 16 << 3 | 7: return ms->config7;
    case 17 << 3: return ms->lladdr >> 4;
    case 18 << 3: return ms->watchlo;
    case 19 << 3: return ms->watchhi;
    case 23 << 3: return ms->debug;
    case 24 << 3: return ms->depc;
    case 26 << 3: return ms->errctl;
    case 28 << 3: case 28 << 3 | 1: case 28 << 3 | 2: case 28 << 3 | 3:
        return ms->taglo[sel];
    case 30 << 3: return ms->errorepc;
    case 31 << 3: return ms->desave;
    }
    return 0;
}

static void cp0_write(u32 rd, u32 sel, u32 v)
{
    struct sim *s = ms->sim;
    switch (rd << 3 | sel) {
    case 0 << 3: ms->index = (ms->index & 0x80000000u) | (v & 15); return;
    case 2 << 3: ms->entrylo0 = v & 0x3fffffffu; return;
    case 3 << 3: ms->entrylo1 = v & 0x3fffffffu; return;
    case 4 << 3: ms->context = (v & 0xff800000u) | (ms->context & 0x7ffff0u); return;
    case 4 << 3 | 2: ms->userlocal = v; return;
    case 5 << 3: ms->pagemask = v & 0x1fffe000u; return;
    case 6 << 3: ms->wired = v & 15; return;
    case 7 << 3: ms->hwrena = v & 0x2000000fu; return;
    case 9 << 3:
        ms->count_base = v;
        ms->count_at = s->cycles;
        timer_plan();
        return;
    case 10 << 3: ms->entryhi = v & 0xffffe0ffu; return;
    case 11 << 3:
        ms->compare = v;
        ms->cause &= ~CA_TI;
        timer_plan();
        core_lines();
        return;
    case 12 << 3: {
        u32 m = status_mask();
        ms->status = (ms->status & ~m) | (v & m);
        s->bus.touched = 1;
        return;
    }
    case 12 << 3 | 1:
        ms->intctl = (ms->intctl & ~(31u << 5)) | (v & (31u << 5));
        return;
    case 12 << 3 | 2: {
        u32 hss = (ms->srsctl >> 26) & 15;
        u32 ess = (v >> 12) & 15, pss = (v >> 6) & 15;
        ms->srsctl = (ms->srsctl & ~(15u << 12 | 15u << 6)) |
                     (ess <= hss ? ess : 0) << 12 | (pss <= hss ? pss : 0) << 6;
        return;
    }
    case 12 << 3 | 3: ms->srsmap = v; return;
    case 13 << 3: {
        u32 m = CA_IV | (1u << 22) | 0x300u | CA_DC;
        u32 old = ms->cause;
        if ((old ^ v) & CA_DC) {
            if (v & CA_DC) {                /* Count stops where it is */
                ms->count_base = count_now();
                ms->cause |= CA_DC;
            } else {
                ms->cause &= ~CA_DC;
                ms->count_at = s->cycles;
            }
            timer_plan();
        }
        ms->cause = (ms->cause & ~m) | (v & m);
        core_lines();
        s->bus.touched = 1;
        return;
    }
    case 14 << 3: ms->epc = v; return;
    case 15 << 3 | 1:
        ms->ebase = (ms->ebase & ~0x3ffff000u) | (v & 0x3ffff000u);
        return;
    case 16 << 3: ms->config0 = (ms->config0 & ~7u) | (v & 7); return;
    case 18 << 3: ms->watchlo = v; return;
    case 19 << 3: ms->watchhi = v & ~7u; return;
    case 23 << 3: ms->debug = (ms->debug & DBG_DM) | (v & ~DBG_DM); return;
    case 24 << 3: ms->depc = v; return;
    case 26 << 3: ms->errctl = v; return;
    case 28 << 3: case 28 << 3 | 1: case 28 << 3 | 2: case 28 << 3 | 3:
        ms->taglo[sel] = v;
        return;
    case 30 << 3: ms->errorepc = v; return;
    case 31 << 3: ms->desave = v; return;
    }
}

static void tlb_write(u32 i)
{
    struct mips_tlbe *t = &ms->tlb[i % MIPS_TLB];
    t->mask = ms->pagemask;
    t->hi = ms->entryhi & ~(ms->pagemask | 0x1f00u);
    t->lo0 = ms->entrylo0 & ~1u;
    t->lo1 = ms->entrylo1 & ~1u;
    t->g = ms->entrylo0 & ms->entrylo1 & 1;
}

static void eret(void)
{
    u32 to;
    if (ms->status & ST_ERL) {
        to = ms->errorepc;
        ms->status &= ~ST_ERL;
    } else {
        to = ms->epc;
        ms->status &= ~ST_EXL;
        u32 hss = (ms->srsctl >> 26) & 15;
        if (hss && !(ms->status & ST_BEV)) {
            u32 pss = (ms->srsctl >> 6) & 15;
            ms->srsctl = (ms->srsctl & ~15u) | pss;
            ms->r = ms->gpr[pss];
        }
    }
    ms->llbit = 0;
    ms->nx = to;
    ms->nnx = to + 4;
    ms->nslot = 0;
    ms->sim->bus.touched = 1;
    if (ms->sim->an)
        an_exc_return(ms->sim);
}

static void cop0(u32 i)
{
    u32 rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
    if (!kernel() && !(ms->status & ST_CU0)) {
        raise_cpu(0);
        return;
    }
    switch (rs) {
    case 0x00:                                  /* MFC0 */
        W(rt, cp0_read(rd, i & 7));
        return;
    case 0x04:                                  /* MTC0 */
        cp0_write(rd, i & 7, R(rt));
        return;
    case 0x0a:                                  /* RDPGPR */
        W(rd, ms->gpr[(ms->srsctl >> 6) & 15][rt]);
        return;
    case 0x0b:                                  /* DI, EI */
        if (rd != 12 || (i & 0x7df) != 0) {
            reserved();
            return;
        }
        W(rt, ms->status);
        if (i & 0x20)
            ms->status |= ST_IE;
        else
            ms->status &= ~ST_IE;
        ms->sim->bus.touched = 1;
        return;
    case 0x0e:                                  /* WRPGPR */
        if (rd)
            ms->gpr[(ms->srsctl >> 6) & 15][rd] = R(rt);
        return;
    }
    if (!(rs & 0x10)) {
        reserved();
        return;
    }
    switch (i & 0x3f) {
    case 0x01: {                                /* TLBR */
        const struct mips_tlbe *t = &ms->tlb[ms->index & 15];
        ms->pagemask = t->mask;
        ms->entryhi = t->hi;
        ms->entrylo0 = t->lo0 | (u32)t->g;
        ms->entrylo1 = t->lo1 | (u32)t->g;
        return;
    }
    case 0x02: tlb_write(ms->index & 15); return;  /* TLBWI */
    case 0x06: tlb_write(cp0_read(1, 0)); return;  /* TLBWR */
    case 0x08: {                                /* TLBP */
        int k = tlb_find(ms->entryhi & ~0x1fffu, ms->entryhi & 0xff);
        ms->index = k < 0 ? 0x80000000u : (u32)k;
        return;
    }
    case 0x18:                                  /* ERET */
        eret();
        return;
    case 0x1f:                                  /* DERET */
        if (!(ms->debug & DBG_DM)) {
            reserved();
            return;
        }
        ms->debug &= ~DBG_DM;
        ms->nx = ms->depc;
        ms->nnx = ms->depc + 4;
        ms->nslot = 0;
        return;
    case 0x20: {                                /* WAIT */
        struct sim *s = ms->sim;
        u32 left;
        if (interrupt_ready())
            return;
        if (sim_next_event(s, &left)) {
            s->cycles += left;
            sim_advance(s, left);
            if (s->cycles >= ms->ti_at)
                timer_fire();
            return;
        }
        sim_end(s, END_IDLE, "waiting for an interrupt that nothing can "
                "raise, at 0x%08x", ms->pc);
        return;
    }
    }
    reserved();
}

/* ---- the DSP ASE's part an RTOS uses ----------------------------------- */

static int dsp_ok(void)
{
    if (!ms->dsp) {
        reserved();
        return 0;
    }
    if (!(ms->status & ST_MX)) {
        raise_exc(EXC_DSPDIS);
        return 0;
    }
    return 1;
}

/* rddsp's and wrdsp's mask, bit by bit: pos, scount, c, ouflag, ccond,
 * EFI */
static u32 dsp_fields(u32 mask)
{
    u32 m = 0;
    if (mask & 1) m |= 0x3fu;
    if (mask & 2) m |= 0x3fu << 7;
    if (mask & 4) m |= 1u << 13;
    if (mask & 8) m |= 0xffu << 16;
    if (mask & 16) m |= 0x0fu << 24;
    if (mask & 32) m |= 1u << 14;
    return m;
}

/* ---- the instructions ---------------------------------------------------- */

static void branch(int taken, u32 target)
{
    if (taken)
        ms->nnx = target;
    ms->nslot = 1;
}

static void branch_likely(int taken, u32 target)
{
    if (taken) {
        ms->nnx = target;
        ms->nslot = 1;
    } else {
        ms->nx = ms->pc + 8;
        ms->nnx = ms->pc + 12;
    }
}

void mips_branch(int taken, u32 target, int likely)
{
    if (likely)
        branch_likely(taken, target);
    else
        branch(taken, target);
}

/* the word at a, without side effects, or ~0 */
static u32 peek(u32 a)
{
    u32 v;
    if (bus_debug_read(&ms->sim->bus, mips_phys(a), 4, &v))
        return ~0u;
    return v;
}

/* An unconditional branch to itself: if its delay slot does nothing (a
 * nop, ssnop or ehb), the run is over unless an interrupt can come. */
static void self_loop(u32 target)
{
    struct sim *s = ms->sim;
    if (target != ms->pc || ms->slot || s->state != RUN)
        return;
    u32 d = peek(ms->pc + 4);
    if (d != 0 && d != 0x40 && d != 0xc0)
        return;
    if (interrupts_on() && (interrupt_ready() || sim_event_coming(s)))
        return;
    sim_end(s, END_IDLE, "a loop at 0x%08x that nothing can interrupt", ms->pc);
}

static void trap_if(int c)
{
    if (c)
        raise_exc(EXC_TR);
}

static u32 acc_of(u32 i, int at)
{
    return (i >> at) & 3;
}

static void hilo_set(u32 ac, u64 v)
{
    ms->lo[ac] = (u32)v;
    ms->hi[ac] = (u32)(v >> 32);
}

static u64 hilo(u32 ac)
{
    return (u64)ms->hi[ac] << 32 | ms->lo[ac];
}

static void special(u32 i)
{
    u32 rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
    u32 sa = (i >> 6) & 31, a = R(rs), b = R(rt);
    switch (i & 0x3f) {
    case 0x00: W(rd, b << sa); return;                          /* SLL */
    case 0x01:                                                  /* MOVF, MOVT */
        if (!(ms->status & ST_CU1) || !ms->fpu) {
            raise_cpu(1);
            return;
        }
        mips_fp_exec(i);
        return;
    case 0x02:                                                  /* SRL, ROTR */
        if (rs & 1)
            W(rd, sa ? b >> sa | b << (32 - sa) : b);
        else
            W(rd, b >> sa);
        return;
    case 0x03: W(rd, (u32)((s32)b >> sa)); return;              /* SRA */
    case 0x04: W(rd, b << (a & 31)); return;                    /* SLLV */
    case 0x06: {                                                /* SRLV, ROTRV */
        u32 n = a & 31;
        if (sa & 1)
            W(rd, n ? b >> n | b << (32 - n) : b);
        else
            W(rd, b >> n);
        return;
    }
    case 0x07: W(rd, (u32)((s32)b >> (a & 31))); return;        /* SRAV */
    case 0x08:                                                  /* JR */
        branch(1, a);
        return;
    case 0x09:                                                  /* JALR */
        W(rd, ms->pc + 8);
        branch(1, a);
        return;
    case 0x0a: if (!b) W(rd, a); return;                        /* MOVZ */
    case 0x0b: if (b) W(rd, a); return;                         /* MOVN */
    case 0x0c: raise_exc(EXC_SYS); return;                      /* SYSCALL */
    case 0x0d: raise_exc(EXC_BP); return;                       /* BREAK */
    case 0x0f: return;                                          /* SYNC */
    case 0x10: {                                                /* MFHI */
        u32 ac = acc_of(i, 21);
        if (ac && !dsp_ok())
            return;
        W(rd, ms->hi[ac]);
        return;
    }
    case 0x11: {                                                /* MTHI */
        u32 ac = acc_of(i, 11);
        if (ac && !dsp_ok())
            return;
        ms->hi[ac] = a;
        return;
    }
    case 0x12: {                                                /* MFLO */
        u32 ac = acc_of(i, 21);
        if (ac && !dsp_ok())
            return;
        W(rd, ms->lo[ac]);
        return;
    }
    case 0x13: {                                                /* MTLO */
        u32 ac = acc_of(i, 11);
        if (ac && !dsp_ok())
            return;
        ms->lo[ac] = a;
        return;
    }
    case 0x18: case 0x19: {                                     /* MULT, MULTU */
        u32 ac = acc_of(i, 11);
        if (ac && !dsp_ok())
            return;
        hilo_set(ac, (i & 1) ? (u64)a * b : (u64)((s64)(s32)a * (s32)b));
        return;
    }
    case 0x1a:                                                  /* DIV */
        /* by zero, and INT_MIN by -1: LO the dividend, HI 0, as QEMU
         * has it (the architecture leaves them unpredictable) */
        if (!b || (a == 0x80000000u && b == 0xffffffffu)) {
            ms->lo[0] = a;
            ms->hi[0] = 0;
        } else {
            ms->lo[0] = (u32)((s32)a / (s32)b);
            ms->hi[0] = (u32)((s32)a % (s32)b);
        }
        return;
    case 0x1b:                                                  /* DIVU */
        if (!b) {
            ms->lo[0] = a;
            ms->hi[0] = 0;
        } else {
            ms->lo[0] = a / b;
            ms->hi[0] = a % b;
        }
        return;
    case 0x20:                                                  /* ADD */
        if (((a + b) ^ a) & ((a + b) ^ b) & 0x80000000u)
            raise_exc(EXC_OV);
        else
            W(rd, a + b);
        return;
    case 0x21: W(rd, a + b); return;                            /* ADDU */
    case 0x22:                                                  /* SUB */
        if ((a ^ b) & (a ^ (a - b)) & 0x80000000u)
            raise_exc(EXC_OV);
        else
            W(rd, a - b);
        return;
    case 0x23: W(rd, a - b); return;                            /* SUBU */
    case 0x24: W(rd, a & b); return;                            /* AND */
    case 0x25: W(rd, a | b); return;                            /* OR */
    case 0x26: W(rd, a ^ b); return;                            /* XOR */
    case 0x27: W(rd, ~(a | b)); return;                         /* NOR */
    case 0x2a: W(rd, (s32)a < (s32)b); return;                  /* SLT */
    case 0x2b: W(rd, a < b); return;                            /* SLTU */
    case 0x30: trap_if((s32)a >= (s32)b); return;               /* TGE */
    case 0x31: trap_if(a >= b); return;                         /* TGEU */
    case 0x32: trap_if((s32)a < (s32)b); return;                /* TLT */
    case 0x33: trap_if(a < b); return;                          /* TLTU */
    case 0x34: trap_if(a == b); return;                         /* TEQ */
    case 0x36: trap_if(a != b); return;                         /* TNE */
    }
    reserved();
}

static void regimm(u32 i)
{
    u32 rs = (i >> 21) & 31, rt = (i >> 16) & 31;
    s32 a = (s32)R(rs);
    u32 t = ms->pc + 4 + (u32)((s32)(s16)i << 2), imm = (u32)(s32)(s16)i;
    switch (rt) {
    case 0x00: branch(a < 0, t); return;                        /* BLTZ */
    case 0x01:                                                  /* BGEZ */
        if (!rs)
            self_loop(t);
        branch(a >= 0, t);
        return;
    case 0x02: branch_likely(a < 0, t); return;                 /* BLTZL */
    case 0x03: branch_likely(a >= 0, t); return;                /* BGEZL */
    case 0x08: trap_if(a >= (s32)imm); return;                  /* TGEI */
    case 0x09: trap_if((u32)a >= imm); return;                  /* TGEIU */
    case 0x0a: trap_if(a < (s32)imm); return;                   /* TLTI */
    case 0x0b: trap_if((u32)a < imm); return;                   /* TLTIU */
    case 0x0c: trap_if(a == (s32)imm); return;                  /* TEQI */
    case 0x0e: trap_if(a != (s32)imm); return;                  /* TNEI */
    case 0x10:                                                  /* BLTZAL */
        W(31, ms->pc + 8);
        branch(a < 0, t);
        return;
    case 0x11:                                                  /* BGEZAL, BAL */
        W(31, ms->pc + 8);
        branch(a >= 0, t);
        return;
    case 0x12:                                                  /* BLTZALL */
        W(31, ms->pc + 8);
        branch_likely(a < 0, t);
        return;
    case 0x13:                                                  /* BGEZALL */
        W(31, ms->pc + 8);
        branch_likely(a >= 0, t);
        return;
    case 0x1c:                                                  /* BPOSGE32 */
        if (dsp_ok())
            branch((ms->dspctl & 0x3f) >= 32, t);
        return;
    case 0x1f: return;                                          /* SYNCI */
    }
    reserved();
}

static void special2(u32 i)
{
    u32 rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
    u32 a = R(rs), b = R(rt), ac = acc_of(i, 11);
    switch (i & 0x3f) {
    case 0x00: case 0x01: case 0x04: case 0x05: {   /* MADD, MADDU, MSUB, MSUBU */
        if (ac && !dsp_ok())
            return;
        u64 p = (i & 1) ? (u64)a * b : (u64)((s64)(s32)a * (s32)b);
        hilo_set(ac, (i & 4) ? hilo(ac) - p : hilo(ac) + p);
        return;
    }
    case 0x02: W(rd, a * b); return;                            /* MUL */
    case 0x20: case 0x21: {                                     /* CLZ, CLO */
        u32 v = (i & 1) ? ~a : a, n = 0;
        while (n < 32 && !(v >> (31 - n) & 1))
            n++;
        W(rd, n);
        return;
    }
    case 0x3f: {                                                /* SDBBP */
        struct sim *s = ms->sim;
        raise_exc(EXC_BP);
        sim_end(s, END_LOCKUP, "sdbbp at 0x%08x: a software breakpoint, "
                "with no debugger to take it", ms->pc);
        return;
    }
    }
    reserved();
}

static void special3(u32 i)
{
    u32 rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
    u32 sa = (i >> 6) & 31, a = R(rs), b = R(rt);
    switch (i & 0x3f) {
    case 0x00: {                                                /* EXT */
        u32 size = rd + 1;
        u32 v = a >> sa;
        W(rt, size >= 32 ? v : v & ((1u << size) - 1));
        return;
    }
    case 0x04: {                                                /* INS */
        if (rd < sa) {
            reserved();
            return;
        }
        u32 size = rd - sa + 1;
        u32 m = (size >= 32 ? ~0u : (1u << size) - 1) << sa;
        W(rt, (b & ~m) | ((a << sa) & m));
        return;
    }
    case 0x0a: {                                                /* LWX, LHX, LBUX */
        if (!dsp_ok())
            return;
        u32 va = a + b, v;
        switch (sa) {
        case 0x00: v = load(va, 4); break;
        case 0x04: v = (u32)(s32)(s16)load(va, 2); break;
        case 0x06: v = load(va, 1); break;
        default: reserved(); return;
        }
        if (!ms->exc)
            W(rd, v);
        return;
    }
    case 0x20:                                                  /* BSHFL */
        switch (sa) {
        case 0x02:                                              /* WSBH */
            W(rd, ((b & 0x00ff00ffu) << 8) | ((b >> 8) & 0x00ff00ffu));
            return;
        case 0x10: W(rd, (u32)(s32)(s8)b); return;              /* SEB */
        case 0x18: W(rd, (u32)(s32)(s16)b); return;             /* SEH */
        }
        break;
    case 0x38:                                                  /* the DSP's EXTR group */
        if (sa == 0x12) {                                       /* RDDSP */
            if (dsp_ok())
                W(rd, ms->dspctl & dsp_fields((i >> 16) & 0x3ff));
            return;
        }
        if (sa == 0x13) {                                       /* WRDSP */
            if (dsp_ok()) {
                u32 m = dsp_fields((i >> 11) & 0x3ff);
                ms->dspctl = (ms->dspctl & ~m) | (a & m);
            }
            return;
        }
        if (dsp_ok())
            mips_dsp_exec(i);
        return;
    case 0x0c: case 0x10: case 0x11: case 0x12: case 0x13:     /* the DSP's */
    case 0x18: case 0x30: case 0x31:
        if (dsp_ok())
            mips_dsp_exec(i);
        return;
    case 0x3b:                                                  /* RDHWR */
        if (!kernel() && !(ms->hwrena >> rd & 1)) {
            reserved();
            return;
        }
        switch (rd) {
        case 0: W(rt, ms->ebase & 0x3ff); return;               /* CPUNum */
        case 1: W(rt, 16); return;                              /* SYNCI_Step */
        case 2: W(rt, count_now()); return;                     /* CC */
        case 3: W(rt, 2); return;                               /* CCRes */
        case 29: W(rt, ms->userlocal); return;                  /* ULR */
        }
        reserved();
        return;
    }
    /* the rest of SPECIAL3 is MT's and EVA's, which a PIC32 does not
     * have */
    reserved();
}

static void exec(u32 i)
{
    u32 op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31;
    u32 a = R(rs), imm = (u32)(s32)(s16)i, uimm = i & 0xffff;
    u32 t = ms->pc + 4 + (imm << 2), va = a + imm, v;
    switch (op) {
    case 0x00: special(i); return;
    case 0x01: regimm(i); return;
    case 0x02: case 0x03: {                                     /* J, JAL */
        u32 to = ((ms->pc + 4) & 0xf0000000u) | (i & 0x03ffffffu) << 2;
        if (op == 3)
            W(31, ms->pc + 8);
        else
            self_loop(to);
        branch(1, to);
        return;
    }
    case 0x04:                                                  /* BEQ */
        if (rs == rt)
            self_loop(t);
        branch(a == R(rt), t);
        return;
    case 0x05: branch(a != R(rt), t); return;                   /* BNE */
    case 0x06: branch((s32)a <= 0, t); return;                  /* BLEZ */
    case 0x07: branch((s32)a > 0, t); return;                   /* BGTZ */
    case 0x08:                                                  /* ADDI */
        if (((a + imm) ^ a) & ((a + imm) ^ imm) & 0x80000000u)
            raise_exc(EXC_OV);
        else
            W(rt, a + imm);
        return;
    case 0x09: W(rt, a + imm); return;                          /* ADDIU */
    case 0x0a: W(rt, (s32)a < (s32)imm); return;                /* SLTI */
    case 0x0b: W(rt, a < imm); return;                          /* SLTIU */
    case 0x0c: W(rt, a & uimm); return;                         /* ANDI */
    case 0x0d: W(rt, a | uimm); return;                         /* ORI */
    case 0x0e: W(rt, a ^ uimm); return;                         /* XORI */
    case 0x0f: W(rt, uimm << 16); return;                       /* LUI */
    case 0x10: cop0(i); return;
    case 0x11: case 0x13: case 0x31: case 0x35: case 0x39: case 0x3d:
        /* COP1, COP1X, LWC1, LDC1, SWC1, SDC1 */
        if (!(ms->status & ST_CU1) || !ms->fpu)
            raise_cpu(1);
        else
            mips_fp_exec(i);
        return;
    case 0x12: case 0x32: case 0x36: case 0x3a: case 0x3e:
        raise_cpu(2);                                           /* COP2 */
        return;
    case 0x14: branch_likely(a == R(rt), t); return;            /* BEQL */
    case 0x15: branch_likely(a != R(rt), t); return;            /* BNEL */
    case 0x16: branch_likely((s32)a <= 0, t); return;           /* BLEZL */
    case 0x17: branch_likely((s32)a > 0, t); return;            /* BGTZL */
    case 0x1c: special2(i); return;
    case 0x1d:                                                  /* JALX */
        if (ms->pic32)
            unmodelled("jalx, to microMIPS,");
        else
            reserved();
        return;
    case 0x1f: special3(i); return;
    case 0x20: v = (u32)(s32)(s8)load(va, 1); break;            /* LB */
    case 0x21: v = (u32)(s32)(s16)load(va, 2); break;           /* LH */
    case 0x23: v = load(va, 4); break;                          /* LW */
    case 0x24: v = load(va, 1); break;                          /* LBU */
    case 0x25: v = load(va, 2); break;                          /* LHU */
    case 0x22: case 0x26: {                                     /* LWL, LWR */
        u32 w = load(va & ~3u, 4), k = va & 3, r = R(rt);
        if (op == 0x22)
            v = (w << (8 * (3 - k))) | (k == 3 ? 0 : r & ((1u << (8 * (3 - k))) - 1));
        else
            v = (w >> (8 * k)) | (k == 0 ? 0 : r & ~(0xffffffffu >> (8 * k)));
        break;
    }
    case 0x30:                                                  /* LL */
        v = load(va, 4);
        if (!ms->exc) {
            u32 pa = 0;
            xlate(va, 0, &pa);
            ms->llbit = 1;
            ms->lladdr = pa;
            ms->llval = v;
        }
        break;
    case 0x28: store(va, 1, R(rt)); return;                     /* SB */
    case 0x29: store(va, 2, R(rt)); return;                     /* SH */
    case 0x2b: store(va, 4, R(rt)); return;                     /* SW */
    case 0x2a: case 0x2e: {                                     /* SWL, SWR */
        u32 k = va & 3, pa, r = R(rt);
        u32 at = op == 0x2a ? va & ~3u : va;
        int n = op == 0x2a ? (int)k + 1 : 4 - (int)k;
        u32 val = op == 0x2a ? r >> (8 * (3 - k)) : r;
        if (xlate(at, 1, &pa) || watched(at, n, 1))
            return;
        if (pwrite(pa, n, val))
            raise_exc(EXC_DBE);
        return;
    }
    case 0x38: {                                                /* SC */
        u32 pa, cur;
        if (va & 3) {
            raise_addr(EXC_ADES, va);
            return;
        }
        if (xlate(va, 1, &pa) || watched(va, 4, 1))
            return;
        int ok = ms->llbit && ms->lladdr == pa && !pread(pa, 4, &cur) &&
                 cur == ms->llval;
        if (ok && pwrite(pa, 4, R(rt))) {
            raise_exc(EXC_DBE);
            return;
        }
        ms->llbit = 0;
        W(rt, ok);
        return;
    }
    case 0x2f:                                                  /* CACHE */
        if (!kernel() && !(ms->status & ST_CU0))
            raise_cpu(0);
        return;
    case 0x33: return;                                          /* PREF */
    default:
        reserved();
        return;
    }
    if (!ms->exc)
        W(rt, v);
}

/* ---- the run ------------------------------------------------------------- */

static void step(struct cpu *c)
{
    ms = (struct mips_state *)c;
    struct sim *s = ms->sim;
    if (s->cycles >= ms->ti_at)
        timer_fire();
    if (interrupt_ready()) {
        take_interrupt();
        return;
    }
    ms->exc = 0;
    ms->watch_stop = 0;
    u32 pc = ms->pc, pa, w;
    if (pc & 3) {
        raise_addr(EXC_ADEL, pc);
        enter(ms->exc_code, 0, (int)((ms->srsctl >> 12) & 15));
        return;
    }
    if (xlate(pc, 2, &pa)) {
        enter(ms->exc_code, 0, (int)((ms->srsctl >> 12) & 15));
        return;
    }
    struct region *r = bus_region(&s->bus, pa, 4);
    if (r) {
        w = mem_rd_le(r->mem + (pa - r->base), 4);
    } else if (bus_read(&s->bus, pa, 4, &w)) {
        u32 base = (ms->status & ST_BEV) ? 0xbfc00380u
                                         : (ms->ebase & 0xfffff000u) + 0x180;
        if (pc == base) {
            sim_end(s, END_LOCKUP, "lockup: the exception vector 0x%08x "
                    "cannot be fetched", pc);
            return;
        }
        raise_exc(EXC_IBE);
        enter(EXC_IBE, 0, (int)((ms->srsctl >> 12) & 15));
        return;
    }
    u8 b[4] = { (u8)w, (u8)(w >> 8), (u8)(w >> 16), (u8)(w >> 24) };
    int br;
    u32 cost = (u32)mips_cost(b, 4, &br);
    s->insns++;
    s->cycles += cost;
    if (s->trace)
        trace_insn(s, pc, &w, 1, 4);
    ms->insn = w;
    ms->nx = ms->npc;
    ms->nnx = ms->npc + 4;
    ms->nslot = 0;
    exec(w);
    if (ms->exc) {
        if (ms->watch_stop) {
            s->insns--;
            s->cycles -= cost;
            return;
        }
        if (s->state != RUN)
            return;                 /* sdbbp, or what is not modelled */
        enter(ms->exc_code, 0, (int)((ms->srsctl >> 12) & 15));
        return;
    }
    if (s->counting)
        sim_advance(s, cost);
    ms->pc = ms->nx;
    ms->npc = ms->nnx;
    ms->slot = ms->nslot;
}

/* ---- the CPU interface --------------------------------------------------- */

/* Power-on: Status with BEV and ERL, the boot vector, the registers 0
 * (the part leaves them undefined; QEMU clears them) */
static void cpu_reset(struct cpu *c)
{
    struct mips_state *k = (struct mips_state *)c;
    ms = k;
    memset(k->gpr, 0, sizeof k->gpr);
    memset(k->hi, 0, sizeof k->hi);
    memset(k->lo, 0, sizeof k->lo);
    memset(k->tlb, 0, sizeof k->tlb);
    for (int i = 0; i < MIPS_TLB; i++)              /* distinct, unmatched */
        k->tlb[i].hi = 0x80000000u + ((u32)i << 13);
    k->dspctl = 0;
    k->r = k->gpr[0];
    k->status = ST_BEV | ST_ERL;
    k->cause = 0;
    k->epc = k->errorepc = k->badvaddr = k->depc = 0;
    k->ebase = 0x80000000u;
    k->intctl = k->pic32 ? 0 : 7u << 29;
    k->srsctl = (k->pic32 ? (u32)(MIPS_SETS - 1) : 0) << 26;
    k->srsmap = 0;
    k->config0 = (k->config0 & ~7u) | 2;
    k->hwrena = k->userlocal = k->wired = k->index = 0;
    k->entryhi = k->entrylo0 = k->entrylo1 = k->pagemask = k->context = 0;
    k->debug = k->errctl = k->desave = k->watchlo = k->watchhi = 0;
    memset(k->taglo, 0, sizeof k->taglo);
    k->llbit = 0;
    mips_fp_reset(k);
    k->compare = 0;
    k->count_base = 0;
    k->count_at = k->sim->cycles;
    k->core_lines = 0;
    k->ip_hw = 0;
    timer_plan();
    k->pc = 0xbfc00000u;
    k->npc = k->pc + 4;
    k->slot = 0;
}

static u32 cpu_pc(struct cpu *c)
{
    return ((struct mips_state *)c)->pc;
}

/* a hardware interrupt line, IP2 to IP7 (compatibility mode) */
static void cpu_interrupt(struct cpu *c, int n)
{
    struct mips_state *k = (struct mips_state *)c;
    if (n >= 2 && n <= 7)
        k->ip_hw |= 1u << (n - 2);
}

static u32 cpu_phys(struct cpu *c, u32 a)
{
    (void)c;
    return mips_phys(a);
}

/* ---- the registers, as GDB numbers them --------------------------------
 *
 * QEMU's stub and gdb's MIPS target without a description: r0-r31 0-31,
 * then Status 32, LO 33, HI 34, BadVAddr 35, Cause 36, PC 37, f0-f31
 * 38-69, FCSR 70, FIR 71, and 72 (the frame pointer, read as 0), all in
 * the `g` packet. */

static int put32(u8 *buf, u32 v)
{
    for (int i = 0; i < 4; i++)
        buf[i] = (u8)(v >> (8 * i));
    return 4;
}

static int cpu_reg_read(struct cpu *c, int n, u8 *buf)
{
    ms = (struct mips_state *)c;
    if (n >= 0 && n < 32)
        return put32(buf, ms->r[n]);
    switch (n) {
    case 32: return put32(buf, ms->status);
    case 33: return put32(buf, ms->lo[0]);
    case 34: return put32(buf, ms->hi[0]);
    case 35: return put32(buf, ms->badvaddr);
    case 36: return put32(buf, cp0_read(13, 0));
    case 37: return put32(buf, ms->pc);
    }
    if (n >= 38 && n <= 69)
        return put32(buf, (u32)ms->fpr[n - 38]);
    if (n == 70)
        return put32(buf, ms->fpu ? ms->fcsr : 0);
    if (n == 71)
        return put32(buf, ms->fpu ? ms->fir : 0);
    if (n == 72)
        return put32(buf, 0);
    return 0;
}

static int cpu_reg_write(struct cpu *c, int n, const u8 *buf)
{
    ms = (struct mips_state *)c;
    u32 v = (u32)buf[0] | (u32)buf[1] << 8 | (u32)buf[2] << 16 |
            (u32)buf[3] << 24;
    if (n >= 0 && n < 32) {
        if (n)
            ms->r[n] = v;
        return 4;
    }
    switch (n) {
    case 32: ms->status = v; ms->sim->bus.touched = 1; return 4;
    case 33: ms->lo[0] = v; return 4;
    case 34: ms->hi[0] = v; return 4;
    case 35: ms->badvaddr = v; return 4;
    case 36: ms->cause = (ms->cause & ~0x0300u) | (v & 0x0300u); return 4;
    case 37:
        ms->pc = v;
        ms->npc = v + 4;
        ms->slot = 0;
        return 4;
    }
    if (n >= 38 && n <= 69) {
        ms->fpr[n - 38] = (ms->fpr[n - 38] & ~0xffffffffull) | v;
        return 4;
    }
    if (n == 70 && ms->fpu)
        ms->fcsr = (ms->fcsr & 0x000c0000u) | (v & ~0x000c0000u);
    if (n >= 70 && n <= 72)
        return 4;
    return 0;
}

static int g_regs[74];

static const int *cpu_gdb_g_regs(struct cpu *c)
{
    (void)c;
    for (int i = 0; i < 73; i++)
        g_regs[i] = i;
    g_regs[73] = -1;
    return g_regs;
}

static const char *cpu_gdb_xml(struct cpu *c, const char *annex)
{
    (void)c;
    (void)annex;
    return 0;
}

static const struct cpu_ops mips_ops = {
    "mips", 8, "a MIPS image",
    cpu_reset, step, cpu_pc, cpu_reg_read, cpu_reg_write,
    cpu_gdb_g_regs, cpu_gdb_xml, 4, cpu_interrupt, 37, 1, cpu_phys,
};

int mips_is(const struct cpu *c)
{
    return c && c->ops == &mips_ops;
}

/* The cores: `pic32mz`, a PIC32MZ EF's (M-class: shadow sets, EIC mode
 * with the EVIC, the DSP ASE's registers, an FPU's CU1 -- the FPU itself
 * not yet), and `24kc`, QEMU malta's (MIPS32r2, no FPU, no DSP, the
 * timer on IP7). The IDs are the parts': PRId and Config1's caches and
 * TLB, which a startup reads to set the caches up. */
struct cpu *mips_create(struct sim *s, const char *model,
                        const struct board_desc *bd)
{
    (void)bd;
    int pic32 = !strcmp(model, "pic32mz");
    if (!pic32 && strcmp(model, "24kc"))
        return 0;
    struct mips_state *k = calloc(1, sizeof *k);
    if (!k)
        die("out of memory");
    k->cpu.ops = &mips_ops;
    k->sim = s;
    k->pic32 = pic32;
    if (pic32) {
        /* M5150 (PRId 0xa7), 16-entry TLB; 16 KiB 4-way I-cache and 4 KiB
         * 4-way D-cache of 16-byte lines; performance counters, watch
         * registers, EJTAG, the FPU. Config3: 8-bit IPL, the MCU ASE,
         * MIPS32 and microMIPS (MIPS32 at reset), UserLocal, DSP and
         * DSPr2, EIC and vectored interrupts. */
        k->prid = 0x0001a700u;
        k->config0 = 0x80000000u | 1u << 10 | 1u << 7;
        k->config1 = 0x80000000u | 15u << 25 | 2u << 22 | 3u << 19 | 3u << 16 |
                     0u << 13 | 3u << 10 | 3u << 7 | 1u << 4 | 1u << 3 |
                     1u << 1 | 1u;
        k->config2 = 0x80000000u;
        k->config3 = 1u << 21 | 1u << 17 | 2u << 14 | 1u << 13 | 1u << 11 |
                     1u << 10 | 1u << 6 | 1u << 5;
        k->dsp = 1;
        k->fpu = 1;
    } else {
        /* the 24Kc: 64 KiB caches... as QEMU's PRId and Config have it */
        k->prid = 0x00019300u;
        k->config0 = 0x80000000u | 1u << 10 | 1u << 7;
        k->config1 = 0x80000000u | 15u << 25 | 2u << 22 | 3u << 19 | 3u << 16 |
                     2u << 13 | 3u << 10 | 3u << 7 | 1u << 3 | 1u << 1;
        k->config2 = 0x80000000u;
        k->config3 = 0;
    }
    ms = k;
    s->cpu = &k->cpu;
    sim_add_dev(s, 0, 0, &ctimer_ops, k);
    k->count_on = 1;
    return &k->cpu;
}
