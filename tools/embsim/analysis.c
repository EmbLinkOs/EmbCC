/* analysis.c -- the step the analyses watch, and the shadow stack of the
 * program's calls (analysis.h).
 *
 * A call and a return are recognized by the instruction, as each core
 * executes it:
 *   - Cortex-M: BL and BLX are calls; BX, POP and LDM with the pc, LDR to
 *     the pc and MOV to the pc may be returns;
 *   - RISC-V: JAL and JALR that link (rd ra or t0) are calls, C.JAL and
 *     C.JALR too; JALR and C.JR that do not link may be returns;
 *   - AVR: CALL, RCALL and ICALL are calls; RET is a return.
 * A call that was taken pushes a frame, which returns to the instruction
 * after it. A transfer that may be a return is one when it lands where a
 * frame returns to: that frame and every frame above it are popped, so a
 * tail call (a branch to another function, which then returns for both)
 * leaves the stack right. One that lands anywhere else is a jump (a
 * switch's table, a call through a pointer that does not link).
 * Exception entry, which no instruction shows, is the core's to tell
 * (an_exc_entry), and its return (an_exc_return) pops back to it, wherever
 * the handler returns to. */
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "avr.h"
#include "cortexm.h"
#include "devices.h"
#include "riscv.h"

enum { K_OTHER, K_CALL, K_RET };

/* ---- the instruction's kind ------------------------------------------- */

static int fetch16(struct sim *s, u32 a, u32 *h)
{
    struct region *r = bus_region(&s->bus, a, 2);
    if (!r)
        return 0;
    *h = mem_rd_le(r->mem + (a - r->base), 2);
    return 1;
}

static int kind_thumb(struct sim *s, u32 pc, int *size)
{
    u32 h, h2;
    *size = 2;
    if (!fetch16(s, pc, &h))
        return K_OTHER;
    if ((h >> 11) >= 0x1d) {
        *size = 4;
        if (!fetch16(s, pc + 2, &h2))
            return K_OTHER;
        if ((h & 0xf800) == 0xf000 && (h2 & 0xd000) == 0xd000)
            return K_CALL;                          /* BL */
        if ((h & 0xffd0) == 0xe890 || (h & 0xffd0) == 0xe910)
            return h2 & 0x8000 ? K_RET : K_OTHER;   /* LDM with the pc */
        if ((h & 0xfff0) == 0xf8d0 || (h & 0xfff0) == 0xf850)
            return (h2 >> 12) == 15 ? K_RET : K_OTHER;  /* LDR pc */
        return K_OTHER;
    }
    if ((h & 0xff87) == 0x4780)
        return K_CALL;                              /* BLX Rm */
    if ((h & 0xff87) == 0x4700 || (h & 0xff00) == 0xbd00 ||
        (h & 0xff87) == 0x4687)
        return K_RET;                               /* BX, POP {pc}, MOV pc */
    return K_OTHER;
}

static int kind_riscv(struct sim *s, u32 pc, int *size)
{
    u32 h, h2;
    *size = 2;
    if (!fetch16(s, pc, &h))
        return K_OTHER;
    if ((h & 3) == 3) {
        *size = 4;
        if (!fetch16(s, pc + 2, &h2))
            return K_OTHER;
        u32 i = h | h2 << 16, op = i & 0x7f, rd = (i >> 7) & 31;
        int link = rd == 1 || rd == 5;
        if (op == 0x6f)                             /* JAL */
            return link ? K_CALL : K_OTHER;
        if (op == 0x67)                             /* JALR */
            return link ? K_CALL : rd == 0 ? K_RET : K_OTHER;
        return K_OTHER;
    }
    u32 q = h & 3, f3 = h >> 13, rs1 = (h >> 7) & 31, rs2 = (h >> 2) & 31;
    if (q == 1 && f3 == 1 && ((struct rv_state *)s->cpu)->xlen == 32)
        return K_CALL;                              /* C.JAL */
    if (q == 2 && f3 == 4 && rs1 && !rs2)
        return (h >> 12) & 1 ? K_CALL : K_RET;      /* C.JALR, C.JR */
    return K_OTHER;
}

static int kind_avr(struct sim *s, u32 pc, int *size)
{
    u32 w;
    *size = 2;
    if (!fetch16(s, pc, &w))
        return K_OTHER;
    if ((w & 0xfe0e) == 0x940e) {                   /* CALL */
        *size = 4;
        return K_CALL;
    }
    if ((w & 0xfe0e) == 0x940c)                     /* JMP */
        *size = 4;
    if ((w & 0xf000) == 0xd000 || w == 0x9509)      /* RCALL, ICALL */
        return K_CALL;
    if (w == 0x9508)                                /* RET */
        return K_RET;
    return K_OTHER;
}

int an_sps(struct analysis *a, u32 *msp, u32 *psp)
{
    if (a->arch == AN_ARM) {
        struct cm_state *k = (struct cm_state *)a->s->cpu;
        *msp = k->psp_active ? k->other_sp : k->R[13];
        *psp = k->psp_active ? k->R[13] : k->other_sp;
        return k->psp_active;
    }
    *msp = an_sp(a);
    *psp = 0;
    return 0;
}

int an_data_sym(struct analysis *a, const char *name, u32 *v)
{
    if (!image_sym(a->img, name, v))
        return 0;
    if (a->arch == AN_AVR && *v < 0x10000)
        *v += AVR_DATA;
    return 1;
}

u32 an_sp(struct analysis *a)
{
    switch (a->arch) {
    case AN_ARM: return ((struct cm_state *)a->s->cpu)->R[13];
    case AN_RV: return (u32)((struct rv_state *)a->s->cpu)->x[2];
    default: return AVR_DATA + ((struct avr_state *)a->s->cpu)->sp;
    }
}

/* An exception's entry lands on a jump to the handler where the vector
 * table holds instructions (the AVR's; RISC-V's vectored mode): the
 * jump's target, or `pc` itself when it is not a jump. */
static u32 vector_target(struct analysis *a, u32 pc)
{
    struct sim *s = a->s;
    u32 h, h2;
    if (!fetch16(s, pc, &h))
        return pc;
    if (a->arch == AN_AVR) {
        if ((h & 0xfe0e) == 0x940c && fetch16(s, pc + 2, &h2))   /* JMP */
            return (((h >> 3 & 0x3e) | (h & 1)) << 16 | h2) * 2;
        if ((h & 0xf000) == 0xc000)                             /* RJMP */
            return pc + 2 + (u32)((s32)((h & 0xfff) << 20) >> 19);
    } else if (a->arch == AN_RV) {
        if ((h & 0xfff) == 0x06f && fetch16(s, pc + 2, &h2)) {   /* JAL x0 */
            u32 i = h | h2 << 16;
            s32 off = (s32)((i >> 31) << 20 | ((i >> 12) & 0xff) << 12 |
                            ((i >> 20) & 1) << 11 | ((i >> 21) & 0x3ff) << 1);
            return pc + (u32)((off << 11) >> 11);
        }
        if ((h & 0xe003) == 0xa001) {                           /* C.J */
            s32 off = (s32)(((h >> 12) & 1) << 11 | ((h >> 11) & 1) << 4 |
                            ((h >> 9) & 3) << 8 | ((h >> 8) & 1) << 10 |
                            ((h >> 7) & 1) << 6 | ((h >> 6) & 1) << 7 |
                            ((h >> 3) & 7) << 1 | ((h >> 2) & 1) << 5);
            return pc + (u32)((off << 20) >> 20);
        }
    }
    return pc;
}

/* ---- functions and call paths ------------------------------------------- */

int an_fn(struct analysis *a, u32 pc)
{
    if (pc - a->fn_lo < a->fn_hi - a->fn_lo)
        return a->fn_idx;
    int f = image_fn(a->img, pc);
    if (f >= 0) {
        a->fn_lo = a->img->fn[f].addr;
        a->fn_hi = a->fn_lo + a->img->fn[f].size;
        a->fn_idx = f;
    }
    return f;
}

static int new_node(struct analysis *a, int parent, int fn)
{
    if (a->nnode == a->capnode) {
        a->capnode = a->capnode ? 2 * a->capnode : 256;
        a->node = realloc(a->node, (size_t)a->capnode * sizeof *a->node);
        if (!a->node)
            die("out of memory");
    }
    struct an_node *n = &a->node[a->nnode];
    memset(n, 0, sizeof *n);
    n->parent = parent;
    n->fn = fn;
    n->child = -1;
    n->sib = -1;
    if (parent >= 0) {
        n->sib = a->node[parent].child;
        a->node[parent].child = a->nnode;
    }
    return a->nnode++;
}

static int child(struct analysis *a, int parent, int fn)
{
    for (int c = a->node[parent].child; c >= 0; c = a->node[c].sib)
        if (a->node[c].fn == fn)
            return c;
    return new_node(a, parent, fn);
}

/* the path the instruction at pc is on: its frame's, or, when the pc has
 * left the frame's function (a tail call, or code no call reached), a
 * path below it */
static int cur_node(struct analysis *a, u32 pc)
{
    int base = a->nfr ? a->fr[a->nfr - 1].node : 0;
    if (a->nfr && a->fr[a->nfr - 1].vec == pc && a->fr[a->nfr - 1].exc)
        return base;                /* the vector's jump: the handler's */
    int fn = an_fn(a, pc);
    if (base == a->cur_base && fn == a->cur_fn)
        return a->cur_node;
    a->cur_base = base;
    a->cur_fn = fn;
    a->cur_node = a->node[base].fn == fn && base ? base : child(a, base, fn);
    return a->cur_node;
}

static void push(struct analysis *a, int parent, u32 ret, u32 target, int exc)
{
    if (a->nfr == a->capfr) {
        if (a->capfr >= (1 << 20))
            return;                 /* deeper than any stack can be */
        a->capfr = a->capfr ? 2 * a->capfr : 256;
        a->fr = realloc(a->fr, (size_t)a->capfr * sizeof *a->fr);
        if (!a->fr)
            die("out of memory");
    }
    struct an_frame *f = &a->fr[a->nfr++];
    f->ret = ret;
    f->sp = an_sp(a);
    f->min = f->sp;
    f->exc = exc;
    f->vec = target;
    if (exc)
        target = vector_target(a, target);
    f->node = child(a, parent, an_fn(a, target));
    a->node[f->node].calls++;
}

/* a transfer that may be a return, to `to`: the frame that returns there,
 * above the innermost exception's, is popped with the frames above it */
static void pop_to(struct analysis *a, int k)
{
    if (a->stk)
        for (int i = a->nfr - 1; i >= k; i--)
            stk_pop(a, i);
    a->nfr = k;
}

static void ret_to(struct analysis *a, u32 to)
{
    for (int i = a->nfr - 1; i >= 0 && !a->fr[i].exc; i--)
        if (a->fr[i].ret == to) {
            pop_to(a, i);
            return;
        }
}

static void follow(struct analysis *a, int node, u32 pc1, int ran)
{
    struct sim *s = a->s;
    if (a->nev) {
        for (int i = 0; i < a->nev; i++) {
            if (a->ev[i].what == EV_ENTRY) {
                push(a, node, a->ev[i].ret, s->cpu->ops->pc(s->cpu),
                     (int)a->ev[i].n + 1);
                node = a->fr[a->nfr - 1].node;
            } else {
                int k = a->nfr - 1;
                while (k >= 0 && !a->fr[k].exc)
                    k--;
                if (k >= 0)
                    pop_to(a, k);
                node = a->nfr ? a->fr[a->nfr - 1].node : 0;
            }
        }
        return;
    }
    if (!ran || pc1 == a->pc0 + (u32)a->size)
        return;
    if (a->kind == K_CALL)
        push(a, node, a->pc0 + (u32)a->size, pc1, 0);
    else if (a->kind == K_RET)
        ret_to(a, pc1);
}

/* ---- the step ------------------------------------------------------------ */

void an_exc_entry(struct sim *s, u32 n, u32 ret)
{
    struct analysis *a = s->an;
    if (a->fault)
        fault_entry(a, n, ret);
    if (a->nev < 4) {
        a->ev[a->nev].what = EV_ENTRY;
        a->ev[a->nev].n = n;
        a->ev[a->nev].ret = ret;
        a->nev++;
    }
}

void an_exc_return(struct sim *s)
{
    struct analysis *a = s->an;
    if (a->nev < 4) {
        a->ev[a->nev].what = EV_RETURN;
        a->nev++;
    }
}

void an_step(struct sim *s)
{
    struct analysis *a = s->an;
    struct cpu *c = s->cpu;
    u32 pc0 = c->ops->pc(c);
    u64 i0 = s->insns, c0 = s->cycles;
    a->pc0 = pc0;
    a->nev = 0;
    if (a->calls) {
        switch (a->arch) {
        case AN_ARM: a->kind = kind_thumb(s, pc0, &a->size); break;
        case AN_RV: a->kind = kind_riscv(s, pc0, &a->size); break;
        default: a->kind = kind_avr(s, pc0, &a->size); break;
        }
    }
    c->ops->step(c);
    int ran = s->insns != i0;
    if (ran && a->cov_path) {
        for (int i = 0; i < a->img->ncode; i++)
            if (pc0 - a->img->code_lo[i] <
                a->img->code_hi[i] - a->img->code_lo[i]) {
                a->cov[i][(pc0 - a->img->code_lo[i]) >> 1]++;
                break;
            }
    }
    if (a->calls) {
        int node = cur_node(a, pc0);
        /* the step's counts to the path it ran on, before the call or
         * return it made */
        a->node[node].insns += s->insns - i0;
        a->node[node].cycles += s->cycles - c0;
        if (a->stk) {
            int entered = 0;
            for (int i = 0; i < a->nev; i++)
                entered |= a->ev[i].what == EV_ENTRY;
            stk_step(a, ran, entered, a->kind != K_CALL);
        }
        follow(a, node, c->ops->pc(c), ran);
    }
    if (s->max_insns && s->insns >= s->max_insns && s->state == RUN)
        sim_end(s, END_BUDGET, "--max-insns: %llu instructions run",
                (unsigned long long)s->insns);
}

struct analysis *an_create(struct sim *s, int calls)
{
    struct analysis *a = calloc(1, sizeof *a);
    if (!a)
        die("out of memory");
    a->s = s;
    a->img = image_open(s->image);
    a->arch = avr_is(s->cpu) ? AN_AVR : riscv_is(s->cpu) ? AN_RV : AN_ARM;
    a->calls = calls;
    a->fn_lo = a->fn_hi = 0;        /* an empty cache */
    a->fn_idx = -1;
    new_node(a, -1, -2);            /* the root: no function */
    a->cur_base = -1;
    s->an = a;
    return a;
}

void an_finish(struct sim *s)
{
    struct analysis *a = s->an;
    if (a->fault)
        fault_end(a);
    if (a->cov_path)
        cov_report(a);
    if (a->prof)
        prof_report(a);
    if (a->stk)
        stk_report(a);
}
