/* riscv.h -- the RISC-V core's state, shared by its files and by the
 * device that is part of it (the CLINT).
 *
 *   riscv.c      the state, memory access, traps, the CSRs, step, reset,
 *                the base and M and A instructions, the compressed
 *                forms, and the CPU interface (registers for GDB too)
 *   riscv-fpu.c  the F and D extensions, on IEEE arithmetic of its own
 *   clint.c      the CLINT: msip, mtimecmp and mtime
 *
 * The core executing is `rs`, set by each entry point of the CPU
 * interface, as the Cortex-M's is `cs`. Registers hold XLEN bits: at
 * RV32 a value is kept zero-extended in its u64. */
#ifndef EMBSIM_RISCV_H
#define EMBSIM_RISCV_H

#include "sim.h"

/* exception causes (mcause without the interrupt bit) */
enum {
    EXC_IFETCH_ACCESS = 1, EXC_ILLEGAL = 2, EXC_BREAKPOINT = 3,
    EXC_LOAD_ALIGN = 4, EXC_LOAD_ACCESS = 5, EXC_STORE_ALIGN = 6,
    EXC_STORE_ACCESS = 7, EXC_ECALL_U = 8, EXC_ECALL_M = 11
};

/* not a trap: a debugger's watchpoint stops the instruction before its
 * access, which is not made, and the instruction is not counted */
#define TRAP_WATCH (~0ull)

/* interrupts: mcause's code, and its bit in mip and mie */
#define IRQ_MSI 3
#define IRQ_MTI 7
#define MIP_MSIP (1u << IRQ_MSI)
#define MIP_MTIP (1u << IRQ_MTI)

#define MSTATUS_MIE  (1u << 3)
#define MSTATUS_MPIE (1u << 7)
#define MSTATUS_MPP  (3u << 11)
#define MSTATUS_FS   (3u << 13)
#define MSTATUS_MPRV (1u << 17)
#define MSTATUS_TW   (1u << 21)

enum { PRV_U = 0, PRV_M = 3 };

/* the F and D extensions' accrued exception flags (fflags) */
#define FF_NX 1u
#define FF_UF 2u
#define FF_OF 4u
#define FF_DZ 8u
#define FF_NV 16u

struct rv_state {
    struct cpu cpu;                 /* the interface; first */
    struct sim *sim;
    int model_xlen;                 /* 32 or 64, or 0: the image's */
    int xlen;

    u64 x[32];
    u64 pc, npc;                    /* this instruction, and the next */
    u64 f[32];                      /* a single NaN-boxed in its double */
    u32 fflags, frm;
    int priv;

    /* the machine-mode CSRs */
    u64 mstatus, mie, mtvec, mepc, mcause, mtval, mscratch;
    u64 medeleg, mideleg, mcounteren, mcountinhibit, menvcfg;
    u64 pmpcfg[16], pmpaddr[64];
    u64 mcycle_off, minstret_off;   /* what a write added to the counts */
    u64 cyc_now, insn_now;          /* the counts before this instruction */

    /* LR's reservation: the address, its width and the value read, which
     * SC compares as QEMU's does */
    int resv;
    u64 resv_addr, resv_val;

    /* A trap raised by the instruction executing: the first wins, and
     * nothing the instruction would still do is done. */
    int trap;
    u64 cause, tval;
    u32 insn;                       /* its bits (16 for a compressed one) */
    int changed;                    /* it wrote a register a new value */

    /* the CLINT's registers (clint.c) */
    u32 msip;
    u64 mtimecmp, mtime_off;
};

extern struct rv_state *rs;

/* a value at the core's width */
static inline u64 rv_xl(u64 v)
{
    return rs->xlen == 32 ? (u64)(u32)v : v;
}

/* a value of the core's width, sign-extended */
static inline s64 rv_sx(u64 v)
{
    return rs->xlen == 32 ? (s64)(s32)(u32)v : (s64)v;
}

static inline void rv_wx(u32 rd, u64 v)
{
    if (rd) {
        v = rv_xl(v);
        if (rs->x[rd] != v)
            rs->changed = 1;
        rs->x[rd] = v;
    }
}

/* riscv.c */
void rv_trap(u64 cause, u64 tval);
void rv_illegal(void);
u64 rv_load(u64 a, int n);
void rv_store(u64 a, int n, u64 v);
u64 rv_mtime(void);
u64 rv_mip(void);

/* riscv-fpu.c: an F or D instruction (load, store, arithmetic); 0 when
 * the opcode is not one */
int rv_fp_exec(u32 i);
/* fflags, frm and fcsr, for the CSR instructions and the debugger */
void rv_fp_dirty(void);

#endif
