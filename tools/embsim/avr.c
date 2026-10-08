/* avr.c -- the AVR core of the ATmega328P: the AVR5 instruction set
 * (the classic core with MUL, MOVW, LPM Rd,Z, JMP and CALL, a 16-bit
 * program counter and no ELPM), SREG's flags exactly, the register file
 * and SP and SREG in the data space, interrupts through the vector table,
 * and SLEEP.
 *
 * Cycles are exact: each instruction's are the datasheet's (cost.h's
 * table), with the dynamic parts added here -- a taken branch 1 more, a
 * skip 1 more for a one-word instruction skipped and 2 for a two-word
 * one, an interrupt's entry 4 (and 4 more from SLEEP). There are no wait
 * states on this part, so the count is what the silicon takes.
 *
 * An instruction a skip passes over is not run and not counted as one
 * (s->skipped counts it: QEMU runs it within its block and counts it).
 * An instruction this part does not have (ELPM, the XMEGA's, SPM, which
 * is not modelled) ends the run as a lockup naming it, rather than doing
 * something no part does. */
#include <stdlib.h>
#include <string.h>

#include "../bench/cost.h"
#include "avr.h"
#include "devices.h"

typedef int8_t s8;

static struct avr_state *as;

/* ---- the data space -------------------------------------------------------- */

static struct bus *bus(void)
{
    return &as->sim->bus;
}

/* a debugger's watchpoint on a data access: the instruction completes,
 * and the debugger is told after it */
static void watched(u16 a, int write)
{
    struct bus *b = bus();
    if (b->nwatch)
        bus_watch_check(b, AVR_DATA + a, 1, write);
}

static u8 dread(u16 a)
{
    u32 v;
    watched(a, 0);
    if (a < 0x20)
        return as->r[a];
    switch (a) {
    case 0x5d: return (u8)as->sp;
    case 0x5e: return (u8)(as->sp >> 8);
    case 0x5f: return as->sreg;
    }
    struct region *r = bus_region(bus(), AVR_DATA + a, 1);
    if (r)
        return r->mem[AVR_DATA + a - r->base];
    if (bus_read(bus(), AVR_DATA + a, 1, &v))
        return 0;
    return (u8)v;
}

static void dwrite(u16 a, u8 v)
{
    watched(a, 1);
    as->changed = 1;
    if (a < 0x20) {
        as->r[a] = v;
        return;
    }
    switch (a) {
    case 0x5d: as->sp = (u16)((as->sp & 0xff00) | v); return;
    case 0x5e: as->sp = (u16)((as->sp & 0x00ff) | v << 8); return;
    case 0x5f:
        as->sreg = v;
        avr_irq_changed(as->sim);
        return;
    }
    struct region *r = bus_region(bus(), AVR_DATA + a, 1);
    if (r) {
        if (!r->rom)
            r->mem[AVR_DATA + a - r->base] = v;
        return;
    }
    bus_write(bus(), AVR_DATA + a, 1, v);
}

static u8 flash_byte(u32 a)
{
    struct region *r = bus_region(bus(), a, 1);
    u32 v;
    if (r)
        return r->mem[a - r->base];
    return bus_read(bus(), a, 1, &v) ? 0xff : (u8)v;
}

static u16 flash_word(u32 w)
{
    u32 a = (w % as->flash_words) * 2;
    return (u16)(flash_byte(a) | flash_byte(a + 1) << 8);
}

static void push(u8 v)
{
    dwrite(as->sp, v);
    as->sp--;
}

static u8 pop(void)
{
    as->sp++;
    return dread(as->sp);
}

/* the return address: big-endian on the stack, the low byte pushed first */
static void push_pc(u32 pc)
{
    push((u8)pc);
    push((u8)(pc >> 8));
}

static u32 pop_pc(void)
{
    u32 hi = pop();
    return (hi << 8 | pop()) % as->flash_words;
}

/* ---- interrupts --------------------------------------------------------- */

void avr_irq_source(struct sim *s, int vec, int (*pending)(void *ctx, int vec),
                    void (*ack)(void *ctx, int vec), void *ctx)
{
    struct avr_state *k = (struct avr_state *)s->cpu;
    if (!avr_is(s->cpu))
        die("an AVR peripheral on a board without an AVR core");
    if (k->nsrc == AVR_IRQS)
        die("too many AVR interrupt sources");
    k->src[k->nsrc].vec = vec;
    k->src[k->nsrc].pending = pending;
    k->src[k->nsrc].ack = ack;
    k->src[k->nsrc].ctx = ctx;
    k->nsrc++;
}

void avr_irq_changed(struct sim *s)
{
    struct avr_state *k = (struct avr_state *)s->cpu;
    int best = 0;
    for (int v = 1; v < AVR_VECTORS && !best; v++)
        if (k->sw_pend >> v & 1)
            best = v;
    for (int i = 0; i < k->nsrc; i++) {
        struct avr_irq *q = &k->src[i];
        if ((!best || q->vec < best) && q->pending(q->ctx, q->vec))
            best = q->vec;
    }
    k->pend = best;
}

/* the interrupt's entry: the return address pushed, I cleared, the
 * vector's flag cleared, and 4 cycles (4 more from SLEEP) */
static void take_interrupt(void)
{
    struct sim *s = as->sim;
    int v = as->pend;
    u32 c = as->sleeping ? 8 : 4;
    as->sleeping = 0;
    push_pc(as->pc);
    as->sreg &= (u8)~SREG_I;
    as->pc = (u32)v * 2;
    as->sw_pend &= ~(1u << v);
    for (int i = 0; i < as->nsrc; i++)
        if (as->src[i].vec == v && as->src[i].ack)
            as->src[i].ack(as->src[i].ctx, v);
    avr_irq_changed(s);
    s->cycles += c;
    if (s->counting)
        sim_advance(s, c);
}

/* ---- flags ---------------------------------------------------------------- */

#define B(x, n) (((x) >> (n)) & 1)
#define NB(x, n) (B(x, n) ^ 1)

static void set_flags(u8 mask, u8 v)
{
    as->sreg = (u8)((as->sreg & ~mask) | (v & mask));
}

/* N, Z, S from the result; V given */
static u8 nzs(u8 r, int v)
{
    u8 f = 0;
    if (r & 0x80)
        f |= SREG_N;
    if (!r)
        f |= SREG_Z;
    if (v)
        f |= SREG_V;
    if (B(r, 7) ^ (v != 0))
        f |= SREG_S;
    return f;
}

static u8 add8(u8 d, u8 s, int c)
{
    u8 r = (u8)(d + s + c);
    int h = (B(d, 3) & B(s, 3)) | (B(s, 3) & NB(r, 3)) | (NB(r, 3) & B(d, 3));
    int cy = (B(d, 7) & B(s, 7)) | (B(s, 7) & NB(r, 7)) | (NB(r, 7) & B(d, 7));
    int v = (B(d, 7) & B(s, 7) & NB(r, 7)) | (NB(d, 7) & NB(s, 7) & B(r, 7));
    set_flags(SREG_H | SREG_S | SREG_V | SREG_N | SREG_Z | SREG_C,
              (u8)(nzs(r, v) | (h ? SREG_H : 0) | (cy ? SREG_C : 0)));
    return r;
}

/* SUB, SUBI, CP, CPI (keep_z 0); SBC, SBCI, CPC (keep_z 1: Z only stays) */
static u8 sub8(u8 d, u8 s, int c, int keep_z)
{
    u8 r = (u8)(d - s - c);
    int h = (NB(d, 3) & B(s, 3)) | (B(s, 3) & B(r, 3)) | (B(r, 3) & NB(d, 3));
    int cy = (NB(d, 7) & B(s, 7)) | (B(s, 7) & B(r, 7)) | (B(r, 7) & NB(d, 7));
    int v = (B(d, 7) & NB(s, 7) & NB(r, 7)) | (NB(d, 7) & B(s, 7) & B(r, 7));
    u8 f = (u8)(nzs(r, v) | (h ? SREG_H : 0) | (cy ? SREG_C : 0));
    if (keep_z && !(as->sreg & SREG_Z))
        f &= (u8)~SREG_Z;
    set_flags(SREG_H | SREG_S | SREG_V | SREG_N | SREG_Z | SREG_C, f);
    return r;
}

static u8 logic(u8 r)
{
    set_flags(SREG_S | SREG_V | SREG_N | SREG_Z, nzs(r, 0));
    return r;
}

/* ASR, LSR, ROR: C from bit 0, V = N ^ C */
static u8 shift_right(u8 d, u8 top)
{
    u8 r = (u8)(d >> 1 | top);
    int c = d & 1, n = B(r, 7);
    set_flags(SREG_S | SREG_V | SREG_N | SREG_Z | SREG_C,
              (u8)(nzs(r, n ^ c) | (c ? SREG_C : 0)));
    return r;
}

/* the multiplies: R1:R0, C from bit 15, Z; FMUL's shifted left once */
static void mul_result(u32 p, int fractional)
{
    u16 r = (u16)p;
    int c = B(r, 15);
    if (fractional)
        r = (u16)(r << 1);
    as->r[0] = (u8)r;
    as->r[1] = (u8)(r >> 8);
    set_flags(SREG_Z | SREG_C, (u8)((r ? 0 : SREG_Z) | (c ? SREG_C : 0)));
}

static int is_two_word(u16 w)
{
    return (w & 0xfe0e) == 0x940c || (w & 0xfe0e) == 0x940e ||  /* jmp call */
           (w & 0xfc0f) == 0x9000;                              /* lds sts */
}

/* ---- the instructions -------------------------------------------------------- */

static int unsupported(u16 w)
{
    sim_end(as->sim, END_LOCKUP,
            "lockup: 0x%04x at 0x%04x is not an ATmega328P instruction", w,
            (unsigned)(as->pc * 2));
    return 0;
}

/* skip the next instruction: the cycles and the count of the skipped */
static void skip(void)
{
    u16 next = flash_word(as->npc);
    int n = is_two_word(next) ? 2 : 1;
    as->npc += (u32)n;
    as->sim->cycles += (u32)n;
    as->sim->skipped++;
}

static u16 rp(int n)
{
    return (u16)(as->r[n] | as->r[n + 1] << 8);
}

static void set_rp(int n, u16 v)
{
    as->r[n] = (u8)v;
    as->r[n + 1] = (u8)(v >> 8);
}

/* LD and ST through X, Y or Z: mode 0 plain, 1 post-increment, 2
 * pre-decrement */
static void ldst(int ptr, int mode, int reg, int store)
{
    u16 a = rp(ptr);
    if (mode == 2)
        a--;
    if (store)
        dwrite(a, as->r[reg]);
    else
        as->r[reg] = dread(a);
    if (mode == 1)
        a++;
    if (mode)
        set_rp(ptr, a);
}

/* 1 when the instruction ran; 0 when it ended the run */
static int exec(u16 w, u16 w2)
{
    int d = (w >> 4) & 31, r = (w & 15) | ((w >> 5) & 16);
    u8 K = (u8)(((w >> 4) & 0xf0) | (w & 15));
    int dh = 16 + ((w >> 4) & 15);              /* r16..r31 forms */
    switch (w >> 12) {
    case 0x0:
        if (w == 0)
            return 1;                           /* NOP */
        switch ((w >> 8) & 0xf) {
        case 0x1:                               /* MOVW */
            set_rp(((w >> 4) & 15) * 2, rp((w & 15) * 2));
            return 1;
        case 0x2:                               /* MULS */
            mul_result((u32)((s8)as->r[16 + ((w >> 4) & 15)] *
                             (s8)as->r[16 + (w & 15)]), 0);
            return 1;
        case 0x3: {                             /* MULSU FMUL FMULS FMULSU */
            int a = 16 + ((w >> 4) & 7), b = 16 + (w & 7);
            s32 p;
            switch (w & 0x88) {
            case 0x00: p = (s8)as->r[a] * (s32)as->r[b]; break;
            case 0x08: p = (s32)as->r[a] * (s32)as->r[b]; break;
            case 0x80: p = (s8)as->r[a] * (s8)as->r[b]; break;
            default: p = (s8)as->r[a] * (s32)as->r[b]; break;
            }
            mul_result((u32)p, (w & 0x88) != 0);
            return 1;
        }
        }
        switch ((w >> 10) & 3) {
        case 1: sub8(as->r[d], as->r[r], as->sreg & SREG_C, 1); return 1;   /* CPC */
        case 2: as->r[d] = sub8(as->r[d], as->r[r], as->sreg & SREG_C, 1); return 1; /* SBC */
        case 3: as->r[d] = add8(as->r[d], as->r[r], 0); return 1;           /* ADD */
        }
        return unsupported(w);
    case 0x1:
        switch ((w >> 10) & 3) {
        case 0:                                 /* CPSE */
            if (as->r[d] == as->r[r])
                skip();
            return 1;
        case 1: sub8(as->r[d], as->r[r], 0, 0); return 1;                    /* CP */
        case 2: as->r[d] = sub8(as->r[d], as->r[r], 0, 0); return 1;         /* SUB */
        default: as->r[d] = add8(as->r[d], as->r[r], as->sreg & SREG_C); return 1; /* ADC */
        }
    case 0x2:
        switch ((w >> 10) & 3) {
        case 0: as->r[d] = logic(as->r[d] & as->r[r]); return 1;            /* AND */
        case 1: as->r[d] = logic(as->r[d] ^ as->r[r]); return 1;            /* EOR */
        case 2: as->r[d] = logic(as->r[d] | as->r[r]); return 1;            /* OR */
        default: as->r[d] = as->r[r]; return 1;                             /* MOV */
        }
    case 0x3: sub8(as->r[dh], K, 0, 0); return 1;                           /* CPI */
    case 0x4: as->r[dh] = sub8(as->r[dh], K, as->sreg & SREG_C, 1); return 1; /* SBCI */
    case 0x5: as->r[dh] = sub8(as->r[dh], K, 0, 0); return 1;               /* SUBI */
    case 0x6: as->r[dh] = logic(as->r[dh] | K); return 1;                   /* ORI */
    case 0x7: as->r[dh] = logic(as->r[dh] & K); return 1;                   /* ANDI */
    case 0x8: case 0xa: {                       /* LDD, STD (Y+q, Z+q) */
        int q = (w & 7) | ((w >> 7) & 0x18) | ((w >> 8) & 0x20);
        u16 a = (u16)(rp(w & 8 ? 28 : 30) + q);
        if (w & 0x200)
            dwrite(a, as->r[d]);
        else
            as->r[d] = dread(a);
        return 1;
    }
    case 0xb:                                   /* IN, OUT */
        if (w & 0x800)
            dwrite((u16)(0x20 + ((w & 15) | ((w >> 5) & 0x30))), as->r[d]);
        else
            as->r[d] = dread((u16)(0x20 + ((w & 15) | ((w >> 5) & 0x30))));
        return 1;
    case 0xc: case 0xd: {                       /* RJMP, RCALL */
        s32 k = (s32)((w & 0xfff) << 20) >> 20;
        if (w & 0x1000)
            push_pc(as->npc);
        as->npc = (u32)((s32)as->pc + 1 + k) % as->flash_words;
        return 1;
    }
    case 0xe: as->r[dh] = K; return 1;          /* LDI */
    case 0xf: {
        int b = w & 7;
        s32 k = (s32)(((w >> 3) & 0x7f) << 25) >> 25;
        switch ((w >> 9) & 7) {
        case 0: case 1:                         /* BRBS */
            if (as->sreg >> b & 1) {
                as->npc = (u32)((s32)as->pc + 1 + k) % as->flash_words;
                as->sim->cycles++;
            }
            return 1;
        case 2: case 3:                         /* BRBC */
            if (!(as->sreg >> b & 1)) {
                as->npc = (u32)((s32)as->pc + 1 + k) % as->flash_words;
                as->sim->cycles++;
            }
            return 1;
        case 4:                                 /* BLD */
            if (w & 8)
                return unsupported(w);
            as->r[d] = (u8)((as->r[d] & ~(1u << b)) |
                            ((as->sreg & SREG_T) ? 1u << b : 0));
            return 1;
        case 5:                                 /* BST */
            if (w & 8)
                return unsupported(w);
            set_flags(SREG_T, as->r[d] >> b & 1 ? SREG_T : 0);
            return 1;
        case 6: case 7:                         /* SBRC, SBRS */
            if (w & 8)
                return unsupported(w);
            if ((as->r[d] >> b & 1) == ((w >> 9) & 1))
                skip();
            return 1;
        }
        return 1;
    }
    }
    /* 1001 */
    switch ((w >> 8) & 0xf) {
    case 0x0: case 0x1:                         /* loads */
        switch (w & 15) {
        case 0x0: as->r[d] = dread(w2); return 1;              /* LDS */
        case 0x1: ldst(30, 1, d, 0); return 1;
        case 0x2: ldst(30, 2, d, 0); return 1;
        case 0x4: case 0x5: {                   /* LPM Rd, Z[+] */
            u16 z = rp(30);
            as->r[d] = flash_byte(z);
            if (w & 1)
                set_rp(30, (u16)(z + 1));
            return 1;
        }
        case 0x9: ldst(28, 1, d, 0); return 1;
        case 0xa: ldst(28, 2, d, 0); return 1;
        case 0xc: ldst(26, 0, d, 0); return 1;
        case 0xd: ldst(26, 1, d, 0); return 1;
        case 0xe: ldst(26, 2, d, 0); return 1;
        case 0xf: as->r[d] = pop(); return 1;                  /* POP */
        }
        return unsupported(w);
    case 0x2: case 0x3:                         /* stores */
        switch (w & 15) {
        case 0x0: dwrite(w2, as->r[d]); return 1;              /* STS */
        case 0x1: ldst(30, 1, d, 1); return 1;
        case 0x2: ldst(30, 2, d, 1); return 1;
        case 0x9: ldst(28, 1, d, 1); return 1;
        case 0xa: ldst(28, 2, d, 1); return 1;
        case 0xc: ldst(26, 0, d, 1); return 1;
        case 0xd: ldst(26, 1, d, 1); return 1;
        case 0xe: ldst(26, 2, d, 1); return 1;
        case 0xf: push(as->r[d]); return 1;                    /* PUSH */
        }
        return unsupported(w);
    case 0x4: case 0x5:
        if ((w & 0xfe0c) == 0x940c) {           /* JMP, CALL */
            u32 k = ((u32)(((w >> 3) & 0x3e) | (w & 1)) << 16 | w2);
            if (w & 2)
                push_pc(as->npc);
            as->npc = k % as->flash_words;
            return 1;
        }
        switch (w & 15) {
        case 0x0: as->r[d] = logic((u8)~as->r[d]);            /* COM */
            set_flags(SREG_C, SREG_C);
            return 1;
        case 0x1: {                             /* NEG */
            u8 x = as->r[d], v = (u8)(0 - x);
            u8 f = nzs(v, v == 0x80);
            if (B(v, 3) | B(x, 3))
                f |= SREG_H;
            if (v)
                f |= SREG_C;
            set_flags(SREG_H | SREG_S | SREG_V | SREG_N | SREG_Z | SREG_C, f);
            as->r[d] = v;
            return 1;
        }
        case 0x2:                               /* SWAP */
            as->r[d] = (u8)(as->r[d] << 4 | as->r[d] >> 4);
            return 1;
        case 0x3: {                             /* INC */
            u8 v = (u8)(as->r[d] + 1);
            set_flags(SREG_S | SREG_V | SREG_N | SREG_Z, nzs(v, v == 0x80));
            as->r[d] = v;
            return 1;
        }
        case 0x5: as->r[d] = shift_right(as->r[d], as->r[d] & 0x80); return 1; /* ASR */
        case 0x6: as->r[d] = shift_right(as->r[d], 0); return 1;           /* LSR */
        case 0x7:                                                          /* ROR */
            as->r[d] = shift_right(as->r[d], (as->sreg & SREG_C) ? 0x80 : 0);
            return 1;
        case 0xa: {                             /* DEC */
            u8 v = (u8)(as->r[d] - 1);
            set_flags(SREG_S | SREG_V | SREG_N | SREG_Z, nzs(v, v == 0x7f));
            as->r[d] = v;
            return 1;
        }
        case 0x8:
            if ((w & 0xff0f) == 0x9408) {       /* BSET, BCLR */
                u8 m = (u8)(1u << ((w >> 4) & 7));
                set_flags(m, (w & 0x80) ? 0 : m);
                if (m == SREG_I && !(w & 0x80))
                    as->inhibit = 1;            /* SEI: one more first */
                avr_irq_changed(as->sim);
                return 1;
            }
            switch (w) {
            case 0x9508:                        /* RET */
                as->npc = pop_pc();
                return 1;
            case 0x9518:                        /* RETI */
                as->npc = pop_pc();
                as->sreg |= SREG_I;
                as->inhibit = 1;
                avr_irq_changed(as->sim);
                return 1;
            case 0x9588: {                      /* SLEEP */
                /* with SMCR.SE, until an interrupt is requested: the
                 * time skips ahead to the device that will request one,
                 * and with none (or I clear) the run is over */
                struct sim *s = as->sim;
                u32 left;
                if (!(dread(0x53) & 1))
                    return 1;
                while (!((as->sreg & SREG_I) && as->pend)) {
                    if (!(as->sreg & SREG_I) || !sim_next_event(s, &left)) {
                        sim_end(s, END_IDLE, "asleep with nothing to wake it, "
                                "at 0x%04x", (unsigned)(as->pc * 2));
                        return 0;
                    }
                    if (!left)
                        left = 1;
                    s->cycles += left;
                    sim_advance(s, left);
                }
                as->sleeping = 1;
                return 1;
            }
            case 0x9598:                        /* BREAK: a NOP without OCD */
            case 0x95a8:                        /* WDR */
                return 1;
            case 0x95c8:                        /* LPM */
                as->r[0] = flash_byte(rp(30));
                return 1;
            }
            return unsupported(w);
        case 0x9:
            if (w == 0x9409) {                  /* IJMP */
                as->npc = rp(30) % as->flash_words;
                return 1;
            }
            if (w == 0x9509) {                  /* ICALL */
                push_pc(as->npc);
                as->npc = rp(30) % as->flash_words;
                return 1;
            }
            return unsupported(w);
        }
        return unsupported(w);
    case 0x6: case 0x7: {                       /* ADIW, SBIW */
        int p = 24 + 2 * ((w >> 4) & 3);
        u16 x = rp(p), k = (u16)((w & 15) | ((w >> 2) & 0x30)), v;
        int c, vf;
        if (w & 0x100) {
            v = (u16)(x - k);
            vf = B(x, 15) & NB(v, 15);
            c = B(v, 15) & NB(x, 15);
        } else {
            v = (u16)(x + k);
            vf = NB(x, 15) & B(v, 15);
            c = NB(v, 15) & B(x, 15);
        }
        u8 f = (u8)((B(v, 15) ? SREG_N : 0) | (v ? 0 : SREG_Z) |
                    (vf ? SREG_V : 0) | (c ? SREG_C : 0) |
                    ((B(v, 15) ^ vf) ? SREG_S : 0));
        set_flags(SREG_S | SREG_V | SREG_N | SREG_Z | SREG_C, f);
        set_rp(p, v);
        return 1;
    }
    case 0x8: case 0xa: {                       /* CBI, SBI */
        u16 a = (u16)(0x20 + ((w >> 3) & 31));
        u8 m = (u8)(1u << (w & 7)), v = dread(a);
        dwrite(a, (w & 0x200) ? (u8)(v | m) : (u8)(v & ~m));
        return 1;
    }
    case 0x9: case 0xb: {                       /* SBIC, SBIS */
        u16 a = (u16)(0x20 + ((w >> 3) & 31));
        if ((dread(a) >> (w & 7) & 1) == ((w >> 9) & 1))
            skip();
        return 1;
    }
    default:                                    /* MUL */
        mul_result((u32)as->r[d] * as->r[r], 0);
        return 1;
    }
}

/* ---- the run --------------------------------------------------------------- */

static void step(struct cpu *c)
{
    as = (struct avr_state *)c;
    struct sim *s = as->sim;
    if (as->pend && (as->sreg & SREG_I) && !as->inhibit) {
        take_interrupt();
        return;
    }
    as->inhibit = 0;
    u16 w = flash_word(as->pc), w2 = 0;
    int size = 1;
    if (is_two_word(w)) {
        w2 = flash_word(as->pc + 1);
        size = 2;
    }
    u8 b[4] = { (u8)w, (u8)(w >> 8), (u8)w2, (u8)(w2 >> 8) };
    int br;
    u32 cost = (u32)avr_cost(b, (size_t)(2 * size), &br);
    u64 c0 = s->cycles;
    s->insns++;
    s->cycles += cost;
    if (s->trace) {
        u32 units[2] = { w, w2 };
        trace_insn(s, as->pc * 2, units, size, 2);
    }
    as->npc = (as->pc + (u32)size) % as->flash_words;
    as->changed = 0;
    if (!exec(w, w2))
        return;
    if (s->counting)
        sim_advance(s, (u32)(s->cycles - c0));
    if (as->npc == as->pc && !as->changed && s->state == RUN &&
        ((w >> 12) == 0xc || (w & 0xfe0e) == 0x940c || (w >> 11) == 0x1e)) {
        /* a jump to itself: the end, unless an interrupt can come */
        u32 left;
        if (!(as->sreg & SREG_I) || (!as->pend && !sim_next_event(s, &left))) {
            as->pc = as->npc;
            sim_end(s, END_IDLE, "a loop at 0x%04x that nothing can interrupt",
                    (unsigned)(as->pc * 2));
            return;
        }
    }
    as->pc = as->npc;
}

/* ---- the CPU interface ------------------------------------------------------- */

/* Power-on: the registers zero (the part leaves them undefined; QEMU
 * clears them), SREG 0, SP at RAMEND, and the program counter at the
 * reset vector. */
static void cpu_reset(struct cpu *c)
{
    struct avr_state *k = (struct avr_state *)c;
    memset(k->r, 0, sizeof k->r);
    k->sreg = 0;
    k->sp = 0x08ff;
    k->pc = 0;
    k->inhibit = k->sleeping = 0;
    k->sw_pend = 0;
    k->pend = 0;
    as = k;
}

static u32 cpu_pc(struct cpu *c)
{
    return ((struct avr_state *)c)->pc * 2;
}

static void cpu_interrupt(struct cpu *c, int n)
{
    struct avr_state *k = (struct avr_state *)c;
    if (n > 0 && n < AVR_VECTORS) {
        k->sw_pend |= 1u << n;
        avr_irq_changed(k->sim);
    }
}

/* ---- the registers, as GDB numbers them ---------------------------------------
 *
 * QEMU's stub, and gdb's AVR target: r0-r31 are 0-31 (a byte each), SREG
 * 32, SP 33 (two bytes) and the PC 34 (four bytes, a byte address), all
 * in the `g` packet. */

static int cpu_reg_read(struct cpu *c, int n, u8 *buf)
{
    struct avr_state *k = (struct avr_state *)c;
    if (n >= 0 && n < 32) {
        buf[0] = k->r[n];
        return 1;
    }
    switch (n) {
    case 32: buf[0] = k->sreg; return 1;
    case 33: buf[0] = (u8)k->sp; buf[1] = (u8)(k->sp >> 8); return 2;
    case 34: {
        u32 pc = k->pc * 2;
        for (int i = 0; i < 4; i++)
            buf[i] = (u8)(pc >> (8 * i));
        return 4;
    }
    }
    return 0;
}

static int cpu_reg_write(struct cpu *c, int n, const u8 *buf)
{
    struct avr_state *k = (struct avr_state *)c;
    if (n >= 0 && n < 32) {
        k->r[n] = buf[0];
        return 1;
    }
    switch (n) {
    case 32:
        k->sreg = buf[0];
        avr_irq_changed(k->sim);
        return 1;
    case 33: k->sp = (u16)(buf[0] | buf[1] << 8); return 2;
    case 34: {
        u32 pc = (u32)buf[0] | (u32)buf[1] << 8 | (u32)buf[2] << 16 |
                 (u32)buf[3] << 24;
        k->pc = (pc / 2) % k->flash_words;
        return 4;
    }
    }
    return 0;
}

static const int g_regs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                              14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                              25, 26, 27, 28, 29, 30, 31, 32, 33, 34, -1 };

static const int *cpu_gdb_g_regs(struct cpu *c)
{
    (void)c;
    return g_regs;
}

/* QEMU's description, word for word: a target.xml that includes
 * avr-cpu.xml, gdb's own file (with its odd sizes for sreg, sp and pc,
 * which gdb's AVR target overrides) */
static char xml_buf[4096];

static const char *cpu_gdb_xml(struct cpu *c, const char *annex)
{
    (void)c;
    if (!strcmp(annex, "target.xml"))
        return "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM "
               "\"gdb-target.dtd\"><target><xi:include href=\"avr-cpu.xml\"/>"
               "</target>";
    if (strcmp(annex, "avr-cpu.xml"))
        return 0;
    strcpy(xml_buf,
           "<?xml version=\"1.0\"?>\n"
           "<!-- Copyright (C) 2018-2019 Free Software Foundation, Inc.\n\n"
           "     Copying and distribution of this file, with or without modification,\n"
           "     are permitted in any medium without royalty provided the copyright\n"
           "     notice and this notice are preserved.  -->\n\n"
           "<!-- Register numbers are hard-coded in order to maintain backward\n"
           "     compatibility with older versions of tools that didn't use xml\n"
           "     register descriptions.  -->\n\n"
           "<!DOCTYPE feature SYSTEM \"gdb-target.dtd\">\n"
           "<feature name=\"org.gnu.gdb.avr.cpu\">\n"
           "  <reg name=\"r0\" bitsize=\"8\" type=\"int\" regnum=\"0\"/>\n");
    for (int i = 1; i < 32; i++) {
        size_t len = strlen(xml_buf);
        snprintf(xml_buf + len, sizeof xml_buf - len,
                 "  <reg name=\"r%d\" bitsize=\"8\" type=\"int\"/>\n", i);
    }
    strcat(xml_buf, "  <reg name=\"sreg\" bitsize=\"8\" type=\"int\"/>\n"
                    "  <reg name=\"sp\" bitsize=\"8\" type=\"int\"/>\n"
                    "  <reg name=\"pc\" bitsize=\"8\" type=\"int\"/>\n"
                    "</feature>\n");
    return xml_buf;
}

/* The register file and SP and SREG in the data space, for a debugger
 * (the core's own accesses do not come through the bus for these). */
static u32 regs_read(void *ctx, u32 off, int n)
{
    struct avr_state *k = ctx;
    u32 v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | (off + (u32)i < 32 ? k->r[off + (u32)i] : 0);
    return v;
}

static void regs_write(void *ctx, u32 off, int n, u32 v)
{
    struct avr_state *k = ctx;
    for (int i = 0; i < n; i++)
        if (off + (u32)i < 32)
            k->r[off + (u32)i] = (u8)(v >> (8 * i));
}

static u32 spsreg_read(void *ctx, u32 off, int n)
{
    struct avr_state *k = ctx;
    u8 b[3] = { (u8)k->sp, (u8)(k->sp >> 8), k->sreg };
    u32 v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | (off + (u32)i < 3 ? b[off + (u32)i] : 0);
    return v;
}

static void spsreg_write(void *ctx, u32 off, int n, u32 v)
{
    struct avr_state *k = ctx;
    for (int i = 0; i < n; i++) {
        u8 x = (u8)(v >> (8 * i));
        switch (off + (u32)i) {
        case 0: k->sp = (u16)((k->sp & 0xff00) | x); break;
        case 1: k->sp = (u16)((k->sp & 0x00ff) | x << 8); break;
        case 2: k->sreg = x; avr_irq_changed(k->sim); break;
        }
    }
}

static const struct dev_ops regs_ops = { "avr-regs", regs_read, regs_write, 0, 0, 0 };
static const struct dev_ops spsreg_ops = { "avr-sp-sreg", spsreg_read, spsreg_write, 0, 0, 0 };

static const struct cpu_ops avr_ops = {
    "avr", 83, "an AVR image",
    cpu_reset, step, cpu_pc, cpu_reg_read, cpu_reg_write,
    cpu_gdb_g_regs, cpu_gdb_xml, 2, cpu_interrupt, 34, 1,
};

int avr_is(const struct cpu *c)
{
    return c && c->ops == &avr_ops;
}

/* the core: the ATmega328P, with its own devices -- the registers, SP and
 * SREG as the data space has them */
struct cpu *avr_create(struct sim *s, const char *model,
                       const struct board_desc *bd)
{
    (void)bd;
    if (strcmp(model, "atmega328p"))
        return 0;
    struct avr_state *k = calloc(1, sizeof *k);
    if (!k)
        die("out of memory");
    k->cpu.ops = &avr_ops;
    k->sim = s;
    k->flash_words = 0x4000;                    /* 32 KiB */
    k->sp = 0x08ff;
    as = k;
    s->cpu = &k->cpu;
    s->count_skips = 1;
    bus_add_device(&s->bus, AVR_DATA, 0x20, &regs_ops, k);
    bus_add_device(&s->bus, AVR_DATA + 0x5d, 3, &spsreg_ops, k);
    return &k->cpu;
}
