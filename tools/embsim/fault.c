/* fault.c -- --fault-report: what a fault was, decoded, when the core
 * takes it.
 *
 * The cores tell an_exc_entry of each exception they enter; a Cortex-M
 * HardFault, MemManage, BusFault or UsageFault, or a RISC-V exception
 * (not an interrupt, an ECALL or an EBREAK), is reported on stderr then,
 * before the handler runs:
 *   - the fault status registers decoded into words (CFSR, HFSR, and
 *     MMFAR and BFAR when they are valid), or mcause and mtval;
 *   - the stacked exception frame (r0-r3, r12, lr, pc, xpsr) and the
 *     other registers;
 *   - the faulting instruction, disassembled, with its symbol and line;
 *   - a backtrace: by the image's .debug_frame when it covers the pc,
 *     through an exception frame where the CFI reaches one; else the
 *     calls EmbSim saw the core make (the shadow stack, analysis.c);
 *   - the handler the core goes to, or that there is none.
 * A lockup, which no handler follows, is reported where the run ends. */
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "cortexm.h"
#include "disasm.h"
#include "riscv.h"

static FILE *out(struct analysis *a)
{
    return a->fault_out ? a->fault_out : stderr;
}

static u32 rd32(struct analysis *a, u32 addr, int *ok)
{
    u32 v = 0;
    if (bus_debug_read(&a->s->bus, addr, 4, &v))
        *ok = 0;
    return v;
}

static u64 rdw(struct analysis *a, u32 addr, int bytes, int *ok)
{
    u64 v = rd32(a, addr, ok);
    if (bytes == 8)
        v |= (u64)rd32(a, addr + 4, ok) << 32;
    return v;
}

/* "fn+0x6 (file.c:12)" */
static const char *where(struct analysis *a, u32 pc, char *b, size_t n)
{
    return image_where(a->img, pc, b, n);
}

/* the instruction at pc, as "0x00000124: 6818      ldr r0, [r3, #0]" */
static void insn_line(struct analysis *a, u32 pc, char *b, size_t n)
{
    struct region *r = bus_region(&a->s->bus, pc, 2);
    char t[96];
    if (!r) {
        snprintf(b, n, "0x%08x: (no memory there)", pc);
        return;
    }
    u32 h1 = mem_rd_le(r->mem + (pc - r->base), 2), h2 = 0;
    struct region *r2 = bus_region(&a->s->bus, pc + 2, 2);
    if (r2)
        h2 = mem_rd_le(r2->mem + (pc + 2 - r2->base), 2);
    if (a->arch == AN_ARM) {
        int sz = dis_thumb(pc, h1, h2, t, sizeof t);
        if (sz == 4)
            snprintf(b, n, "0x%08x: %04x %04x  %s", pc, h1, h2, t);
        else
            snprintf(b, n, "0x%08x: %04x       %s", pc, h1, t);
    } else {
        int xlen = ((struct rv_state *)a->s->cpu)->xlen;
        if ((h1 & 3) == 3) {
            dis_riscv(pc, h1 | h2 << 16, xlen, t, sizeof t);
            snprintf(b, n, "0x%08x: %08x  %s", pc, h1 | h2 << 16, t);
        } else {
            dis_riscv(pc, h1, xlen, t, sizeof t);
            snprintf(b, n, "0x%08x: %04x      %s", pc, h1, t);
        }
    }
}

/* ---- the backtrace -------------------------------------------------------- */

struct regs {
    u64 r[32];
    u32 valid;                      /* which of them are known */
};

/* the calls EmbSim saw: the pc's function, then each frame's return */
static void shadow_bt(struct analysis *a, u32 pc)
{
    char w[256];
    FILE *o = out(a);
    fprintf(o, "  backtrace (the calls EmbSim saw the core make):\n");
    fprintf(o, "    #0  0x%08x in %s\n", pc, where(a, pc, w, sizeof w));
    int k = 1;
    for (int i = a->nfr - 1; i >= 0 && k < 64; i--, k++) {
        const struct an_frame *f = &a->fr[i];
        fprintf(o, "    #%-2d 0x%08x in %s%s\n", k, f->ret, where(a, f->ret, w, sizeof w),
                f->exc ? "  <- interrupted here" : "");
    }
}

/* By the CFI: from the registers and the pc at the fault, frame by
 * frame, through an exception frame on a Cortex-M where EXC_RETURN is
 * the return address. When no FDE covers the faulting pc itself, the
 * shadow stack's calls instead. */
static void cfi_bt(struct analysis *a, struct regs *g, u32 pc, int sp_reg, int bytes)
{
    FILE *o = out(a);
    char w[256];
    int exact = 1;                  /* pc is the instruction, not a return */
    struct cfi_row first;
    if (!a->img->frame || !cfi_at(a->img, pc, &first)) {
        shadow_bt(a, pc);
        return;
    }
    fprintf(o, "  backtrace (.debug_frame):\n");
    for (int k = 0; k < 64; k++) {
        fprintf(o, "    #%-2d 0x%08x in %s\n", k, pc, where(a, pc, w, sizeof w));
        struct cfi_row row;
        if (!cfi_at(a->img, exact ? pc : pc - 1, &row)) {
            fprintf(o, "        (no call frame information for 0x%08x: the "
                       "unwinding stops here)\n", pc);
            return;
        }
        if (row.cfa_reg >= 32 || !(g->valid >> row.cfa_reg & 1))
            return;
        u32 cfa = (u32)(g->r[row.cfa_reg] + (u64)row.cfa_off);
        struct regs nx = *g;
        int ok = 1;
        for (int r = 0; r < 32; r++) {
            switch (row.how[r]) {
            case CFI_OFFSET:
                nx.r[r] = rdw(a, cfa + (u32)row.off[r], bytes, &ok);
                nx.valid |= 1u << r;
                break;
            case CFI_VALOFF:
                nx.r[r] = cfa + (u32)row.off[r];
                nx.valid |= 1u << r;
                break;
            case CFI_REG:
                nx.r[r] = row.off[r] >= 0 && row.off[r] < 32 ? g->r[row.off[r]] : 0;
                break;
            case CFI_UNDEF:
                nx.valid &= ~(1u << r);
                break;
            }
        }
        if (!ok || row.ra >= 32 || !(nx.valid >> row.ra & 1))
            return;
        u32 ra = (u32)nx.r[row.ra];
        nx.r[sp_reg] = cfa;
        exact = 0;
        if (a->arch == AN_ARM && ra >= 0xf0000000u) {
            /* an exception's return: the frame its entry stacked, on the
             * stack EXC_RETURN names, holds the interrupted code's pc */
            struct cm_state *c = (struct cm_state *)a->s->cpu;
            u32 sp = ra & 4 ? (c->psp_active ? c->R[13] : c->other_sp) : cfa;
            static const int at[7] = { 0, 1, 2, 3, 12, 14, 15 };
            u32 v[8];
            for (int i = 0; i < 8; i++)
                v[i] = rd32(a, sp + 4 * (u32)i, &ok);
            if (!ok)
                return;
            for (int i = 0; i < 7; i++)
                nx.r[at[i]] = v[i];
            nx.r[13] = sp + (ra & 0x10 ? 0x20u : 0x68u) + (v[7] >> 9 & 1 ? 4u : 0u);
            nx.valid = 0xffff;
            fprintf(o, "        <- an exception's entry (EXC_RETURN 0x%08x): the code "
                       "it interrupted\n", ra);
            ra = v[6];
            exact = 1;
        }
        if (a->arch == AN_ARM)
            ra &= ~1u;
        if (!ra || ra == 0xfffffffeu || (ra == pc && cfa == (u32)g->r[sp_reg]))
            return;
        pc = ra;
        *g = nx;
    }
}

/* ---- the Cortex-M ------------------------------------------------------- */

static const struct { u32 bit; const char *name, *what; } cfsr_bits[] = {
    { 0, "IACCVIOL", "an instruction fetch the MPU refused" },
    { 1, "DACCVIOL", "a data access the MPU refused" },
    { 3, "MUNSTKERR", "a MemManage fault unstacking an exception's frame" },
    { 4, "MSTKERR", "a MemManage fault stacking an exception's frame" },
    { 5, "MLSPERR", "a MemManage fault in the FPU's lazy state preservation" },
    { 8, "IBUSERR", "a bus error fetching an instruction" },
    { 9, "PRECISERR", "a precise data bus error" },
    { 10, "IMPRECISERR", "an imprecise data bus error" },
    { 11, "UNSTKERR", "a bus error unstacking an exception's frame" },
    { 12, "STKERR", "a bus error stacking an exception's frame" },
    { 13, "LSPERR", "a bus error in the FPU's lazy state preservation" },
    { 16, "UNDEFINSTR", "an undefined instruction" },
    { 17, "INVSTATE", "an instruction in an invalid state (the Thumb bit clear: a branch to an even address, or a vector without bit 0)" },
    { 18, "INVPC", "an invalid EXC_RETURN in the pc" },
    { 19, "NOCP", "a coprocessor instruction with the coprocessor off or absent (the FPU off in CPACR?)" },
    { 24, "UNALIGNED", "an unaligned access" },
    { 25, "DIVBYZERO", "a divide by zero (CCR.DIV_0_TRP)" },
};

static const char *const exc_name[7] = { 0, 0, "NMI", "HardFault", "MemManage",
                                         "BusFault", "UsageFault" };

static void arm_fault(struct analysis *a, u32 n, u32 ret)
{
    struct cm_state *c = (struct cm_state *)a->s->cpu;
    FILE *o = out(a);
    char w[256], b[256];
    fprintf(o, "embsim: fault: %s (exception %u) at 0x%08x, in %s\n", exc_name[n], n,
            ret, where(a, ret, w, sizeof w));
    if (n == EXC_HARD) {
        fprintf(o, "  HFSR  0x%08x", c->hfsr);
        if (c->hfsr & (1u << 30))
            fprintf(o, "  FORCED: a configurable fault escalated to HardFault (its "
                       "handler disabled, or not able to preempt)");
        if (c->hfsr & 2)
            fprintf(o, "  VECTTBL: a bus error reading the vector table");
        fprintf(o, "\n");
    }
    fprintf(o, "  CFSR  0x%08x\n", c->cfsr);
    for (size_t i = 0; i < sizeof cfsr_bits / sizeof cfsr_bits[0]; i++)
        if (c->cfsr >> cfsr_bits[i].bit & 1) {
            fprintf(o, "        %s: %s", cfsr_bits[i].name, cfsr_bits[i].what);
            if (cfsr_bits[i].bit == 9 && (c->cfsr & (1u << 15)))
                fprintf(o, " at 0x%08x", c->bfar);
            if (cfsr_bits[i].bit == 1 && (c->cfsr & (1u << 7)))
                fprintf(o, " at 0x%08x", c->mmfar);
            fprintf(o, "\n");
        }
    if (c->cfsr & (1u << 15))
        fprintf(o, "  BFAR  0x%08x (BFARVALID)\n", c->bfar);
    if (c->cfsr & (1u << 7))
        fprintf(o, "  MMFAR 0x%08x (MMARVALID)\n", c->mmfar);
    insn_line(a, ret, b, sizeof b);
    fprintf(o, "  instruction %s\n", b);
    /* the frame the entry stacked: on the stack EXC_RETURN names */
    u32 lr = c->R[14];
    int psp = (lr & 4) != 0;
    u32 sp = psp ? (c->psp_active ? c->R[13] : c->other_sp) : c->R[13];
    int ok = 1;
    u32 v[8];
    for (int i = 0; i < 8; i++)
        v[i] = rd32(a, sp + 4 * (u32)i, &ok);
    fprintf(o, "  stacked frame at 0x%08x (%s):\n", sp, psp ? "PSP" : "MSP");
    fprintf(o, "    r0  0x%08x  r1  0x%08x  r2  0x%08x  r3   0x%08x\n", v[0], v[1], v[2], v[3]);
    fprintf(o, "    r12 0x%08x  lr  0x%08x  pc  0x%08x  xpsr 0x%08x\n", v[4], v[5], v[6], v[7]);
    fprintf(o, "    lr: %s\n", where(a, v[5] & ~1u, w, sizeof w));
    fprintf(o, "  r4  0x%08x  r5  0x%08x  r6  0x%08x  r7   0x%08x\n", c->R[4], c->R[5],
            c->R[6], c->R[7]);
    fprintf(o, "  r8  0x%08x  r9  0x%08x  r10 0x%08x  r11  0x%08x\n", c->R[8], c->R[9],
            c->R[10], c->R[11]);
    u32 frame_sp = sp + (lr & 0x10 ? 0x20u : 0x68u) + (v[7] >> 9 & 1 ? 4u : 0u);
    if (ok) {
        struct regs g;
        memset(&g, 0, sizeof g);
        for (int i = 4; i < 12; i++)
            g.r[i] = c->R[i];
        g.r[0] = v[0]; g.r[1] = v[1]; g.r[2] = v[2]; g.r[3] = v[3];
        g.r[12] = v[4]; g.r[14] = v[5]; g.r[15] = v[6];
        g.r[13] = frame_sp;
        g.valid = 0xffff;
        cfi_bt(a, &g, v[6] & ~1u, 13, 4);
    } else
        shadow_bt(a, ret);
    u32 vec = 0;
    ok = 1;
    vec = rd32(a, c->vtor + 4 * n, &ok);
    int f = image_fn(a->img, vec & ~1u);
    if (!ok)
        fprintf(o, "  handler: the vector table cannot be read at 0x%08x\n", c->vtor + 4 * n);
    else if (!(vec & 1))
        fprintf(o, "  handler: none -- the vector is 0x%08x, without the Thumb bit: "
                   "the core cannot run it (it will fault again)\n", vec);
    else if (f < 0 || a->img->fn[f].addr != (vec & ~1u))
        fprintf(o, "  handler: none -- the vector is 0x%08x, which is no function's "
                   "start (%s)\n", vec, where(a, vec & ~1u, w, sizeof w));
    else
        fprintf(o, "  handler: %s at 0x%08x\n", a->img->fn[f].name, vec & ~1u);
}

/* ---- RISC-V ------------------------------------------------------------- */

static const char *const rv_cause[8] = {
    "instruction address misaligned", "instruction access fault",
    "illegal instruction", "breakpoint", "load address misaligned",
    "load access fault", "store/AMO address misaligned", "store/AMO access fault" };

static const char *const xn[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1",
    "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };

static void rv_fault(struct analysis *a, u32 cause, u32 epc)
{
    struct rv_state *r = (struct rv_state *)a->s->cpu;
    FILE *o = out(a);
    char w[256], b[256];
    fprintf(o, "embsim: fault: %s (mcause %u) at 0x%08x, in %s\n", rv_cause[cause],
            cause, epc, where(a, epc, w, sizeof w));
    u64 tv = r->mtval;
    if (cause == 2)
        fprintf(o, "  mtval 0x%08llx: the instruction's bits\n", (unsigned long long)tv);
    else if (cause == 0 || cause == 1)
        fprintf(o, "  mtval 0x%08llx: the address fetched\n", (unsigned long long)tv);
    else
        fprintf(o, "  mtval 0x%08llx: the address %s\n", (unsigned long long)tv,
                cause <= 5 ? "loaded from" : "stored to");
    insn_line(a, epc, b, sizeof b);
    fprintf(o, "  instruction %s\n", b);
    fprintf(o, "  registers:\n");
    for (int i = 0; i < 32; i += 4) {
        fprintf(o, "   ");
        for (int k = i; k < i + 4; k++)
            fprintf(o, " %-4s 0x%0*llx", xn[k], r->xlen / 4, (unsigned long long)r->x[k]);
        fprintf(o, "\n");
    }
    {
        struct regs g;
        memset(&g, 0, sizeof g);
        for (int i = 0; i < 32; i++)
            g.r[i] = r->x[i];
        g.valid = 0xffffffffu;
        cfi_bt(a, &g, epc, 2, r->xlen / 8);
    }
    u32 base = (u32)(r->mtvec & ~(u64)3);
    int f = image_fn(a->img, base);
    if (!bus_region(&a->s->bus, base, 2))
        fprintf(o, "  handler: none -- mtvec is 0x%08x, where there is nothing to "
                   "fetch: the core locks up\n", base);
    else
        fprintf(o, "  handler: mtvec 0x%08x, %s\n", base,
                f >= 0 ? a->img->fn[f].name : "(no symbol)");
}

/* ---- entry points ---------------------------------------------------------- */

void fault_entry(struct analysis *a, u32 n, u32 ret)
{
    if (a->arch == AN_ARM && n >= EXC_HARD && n <= EXC_USAGE) {
        arm_fault(a, n, ret);
        a->faults++;
    } else if (a->arch == AN_RV && !(n & 0x80000000u) && n < 8 && n != 3) {
        rv_fault(a, n, ret);
        a->faults++;
    }
}

void fault_end(struct analysis *a)
{
    struct sim *s = a->s;
    if (s->state != END_LOCKUP || a->ovf)
        return;
    FILE *o = out(a);
    char w[256], b[256];
    u32 pc = s->cpu->ops->pc(s->cpu);
    fprintf(o, "embsim: the core locked up at 0x%08x, in %s%s\n", pc,
            where(a, pc, w, sizeof w), a->faults ? ": the fault above had no handler" : "");
    if (a->faults)
        return;
    if (a->arch != AN_AVR) {
        insn_line(a, pc, b, sizeof b);
        fprintf(o, "  instruction %s\n", b);
    }
    shadow_bt(a, pc);
}
