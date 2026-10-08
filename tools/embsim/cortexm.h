/* cortexm.h -- the Cortex-M core's state, shared by its files and by the
 * devices that are part of it (the system control space, SysTick).
 *
 *   cortexm.c        the state, memory access, exceptions, step, reset,
 *                    and the CPU interface (registers for GDB too)
 *   cortexm-thumb.c  the Thumb instruction set, 16- and 32-bit
 *   cortexm-fpu.c    the floating-point unit (FPv4-SP, FPv5)
 *   scs.c            the system control space: NVIC and SCB registers
 *
 * The core executing is `cs`. Each entry point of the CPU interface sets
 * it, so two cores would each have their own state; the instruction
 * code reads its registers as cs->R[n] and so on. Not part of sim.h:
 * nothing but the Cortex-M's own files has any business with these. */
#ifndef EMBSIM_CORTEXM_H
#define EMBSIM_CORTEXM_H

#include "sim.h"

enum { ARCH_V6M, ARCH_V7M };
enum { FPU_NONE, FPU_SP, FPU_DP };

struct cpu_model {
    const char *name;
    int arch, dsp, fpu;
    u32 cpuid;
};

/* exception numbers */
enum { EXC_RESET = 1, EXC_NMI, EXC_HARD, EXC_MEM, EXC_BUS, EXC_USAGE,
       EXC_SVC = 11, EXC_DEBUG, EXC_PENDSV = 14, EXC_SYSTICK };

/* CFSR bits */
#define UFSR_UNDEFINSTR (1u << 16)
#define UFSR_INVSTATE   (1u << 17)
#define UFSR_INVPC      (1u << 18)
#define UFSR_NOCP       (1u << 19)
#define UFSR_UNALIGNED  (1u << 24)
#define UFSR_DIVBYZERO  (1u << 25)
#define BFSR_IBUSERR    (1u << 8)
#define BFSR_PRECISERR  (1u << 9)

#define NEXC 256                    /* 16 system exceptions + 240 IRQs */

/* fault_exc's value for an access a debugger's watchpoint stops before */
#define FAULT_WATCH (-1)

struct cm_state {
    struct cpu cpu;                 /* the interface; first */
    struct sim *sim;
    const struct cpu_model *model;
    int prio_bits;                  /* NVIC priority bits (the board's) */

    /* the registers */
    u32 R[16];                      /* R[13] is the active stack pointer */
    u32 other_sp;                   /* the banked one */
    int psp_active;                 /* R[13] is PSP */
    int N, Z, C, V, Q;
    u32 GE;
    u32 itstate;
    int tbit;
    u32 ipsr;                       /* 0 in Thread mode */
    u32 primask, faultmask, basepri, control;
    u32 S[64];                      /* s0..s31 (d0..d15), and d16..d31 */
    u32 fpscr;
    int excl_valid;
    u32 excl_addr;

    u32 pc, npc;                    /* this instruction, and the next */
    int pc_written;
    u32 exc_return_value;
    int exc_return_pending;

    /* A fault raised by the instruction executing: the first one wins,
     * and every access after it is suppressed, so the instruction's
     * partial work is discarded with the register snapshot (step). */
    int fault_exc;
    u32 fault_bits, fault_addr;
    int fault_addr_valid;
    int executing;                  /* in an instruction (not stacking) */

    /* the NVIC and the SCB */
    u8 pend[NEXC], active[NEXC], irq_en[NEXC];
    u8 prio[NEXC];                  /* as programmed, top bits significant */
    int npend;
    u32 vtor, aircr_prigroup, ccr, shcsr, cfsr, hfsr, mmfar, bfar, scr;
    u32 cpacr, fpccr, fpcar, fpdscr;
    u32 demcr;
};

extern struct cm_state *cs;

/* cortexm.c */
void cm_undef(void);
u32 cm_ld_bus(u32 a, int n);
void cm_st_bus(u32 a, int n, u32 v);
void cm_bx_to(u32 a);
u32 cm_xpsr(void);
void cm_set_apsr(u32 v);
int cm_privileged(void);
void cm_use_psp(int psp);
u32 cm_get_msp(void);
u32 cm_get_psp(void);
void cm_set_msp(u32 v);
void cm_set_psp(u32 v);
void cm_set_pend(int n);
void cm_clr_pend(int n);
u32 cm_prio_mask(void);
int cm_exec_prio(void);
int cm_cond_passed(u32 cond);
void cm_it_advance(void);
int cm_wfx_idle(void);
void cm_semihost(void);

/* cortexm-thumb.c */
void cm_exec16(u32 h);
void cm_exec32(u32 h1, u32 h2);

/* cortexm-fpu.c */
int cm_fp_begin(void);
void cm_vfp_ldst(u32 h1, u32 h2);
void cm_vfp_xfer(u32 h1, u32 h2);
void cm_vfp_dp(u32 h1, u32 h2);

/* ---- the hot helpers, inline ---------------------------------------- */

/* a fault of the instruction executing: the first one wins */
static inline void cm_fault_raise(int exc, u32 bits)
{
    if (cs->fault_exc)
        return;
    /* ARMv6-M has no configurable faults: each is a HardFault */
    if (cs->model->arch == ARCH_V6M && exc >= EXC_MEM && exc <= EXC_USAGE)
        exc = EXC_HARD;
    cs->fault_exc = exc;
    cs->fault_bits = bits;
}

/* The core's loads and stores: plain memory directly, the rest (bit-band,
 * devices, a bus error) and every access while a watchpoint is set
 * through the bus. Nothing is accessed once the instruction faulted. A
 * store to the address LDREX marked clears the local monitor. */
static inline u32 cm_ld(u32 a, int n)
{
    if (cs->fault_exc)
        return 0;
    struct bus *b = &cs->sim->bus;
    if (!b->nwatch) {
        struct region *r = bus_region(b, a, (u32)n);
        if (r)
            return mem_rd_le(r->mem + (a - r->base), n);
    }
    return cm_ld_bus(a, n);
}

static inline void cm_st(u32 a, int n, u32 v)
{
    if (cs->fault_exc)
        return;
    struct bus *b = &cs->sim->bus;
    if (!b->nwatch) {
        struct region *r = bus_region(b, a, (u32)n);
        if (r) {
            if (cs->excl_valid && a - cs->excl_addr < 4)
                cs->excl_valid = 0;
            if (!r->rom)
                mem_wr_le(r->mem + (a - r->base), n, v);
            return;
        }
    }
    cm_st_bus(a, n, v);
}

/* An access an instruction makes: v6-M faults on every unaligned one;
 * v7-M on the ones that must be aligned (multiple, dual, exclusive),
 * and on the rest only with CCR.UNALIGN_TRP. */
static inline int cm_aligned_ok(u32 a, int n, int must)
{
    if ((a & (u32)(n - 1)) == 0)
        return 1;
    if (cs->model->arch == ARCH_V6M || must || (cs->ccr & 8)) {
        cm_fault_raise(EXC_USAGE, UFSR_UNALIGNED);
        return 0;
    }
    return 1;
}

static inline u32 cm_mem_ld(u32 a, int n, int must)
{
    return cm_aligned_ok(a, n, must) ? cm_ld(a, n) : 0;
}

static inline void cm_mem_st(u32 a, int n, u32 v, int must)
{
    if (cm_aligned_ok(a, n, must))
        cm_st(a, n, v);
}

static inline u32 cm_reg(int n)
{
    return n == 15 ? cs->pc + 4 : cs->R[n];
}

static inline void cm_set_sp(u32 v)
{
    cs->R[13] = v & ~3u;
}

static inline void cm_branch_to(u32 a)  /* BranchWritePC */
{
    cs->npc = a & ~1u;
    cs->pc_written = 1;
}

static inline void cm_set_reg(int n, u32 v)     /* with ALUWritePC */
{
    if (n == 15)
        cm_branch_to(v);
    else if (n == 13)
        cm_set_sp(v);
    else
        cs->R[n] = v;
}

static inline int cm_in_it(void)
{
    return (cs->itstate & 0xf) != 0;
}

#endif
