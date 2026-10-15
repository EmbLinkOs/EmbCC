/* mips.h -- the MIPS32 core's state, shared by its files and by the
 * interrupt controller that drives it in EIC mode (a PIC32's EVIC).
 *
 *   mips.c       the core: MIPS32 Release 2, little-endian, with branch
 *                delay slots; coprocessor 0 (exceptions, interrupts in
 *                compatibility, vectored and EIC modes, shadow register
 *                sets, the core timer, the TLB); the DSP ASE's
 *                accumulators and DSPControl; step, reset, and the CPU
 *                interface (registers for GDB too)
 *
 * The core executing is `ms`, set by each entry point of the CPU
 * interface, as the Cortex-M's is `cs`. The bus is physical: kseg0 and
 * kseg1 (0x80000000 to 0xbfffffff) reach its low 512 MiB, and the rest
 * of the space goes through the TLB, so an image's load addresses
 * (virtual, in kseg0 and kseg1) are translated by mips_phys. */
#ifndef EMBSIM_MIPS_H
#define EMBSIM_MIPS_H

#include "sim.h"

/* the register sets: the normal one and seven shadow sets, a PIC32MZ's */
#define MIPS_SETS 8
#define MIPS_TLB 16

/* exception codes (Cause.ExcCode) */
enum {
    EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4,
    EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7, EXC_SYS = 8, EXC_BP = 9,
    EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15,
    EXC_DSPDIS = 26
};

/* Status */
#define ST_IE   (1u << 0)
#define ST_EXL  (1u << 1)
#define ST_ERL  (1u << 2)
#define ST_UM   (1u << 4)
#define ST_NMI  (1u << 19)
#define ST_SR   (1u << 20)
#define ST_BEV  (1u << 22)
#define ST_MX   (1u << 24)
#define ST_CU0  (1u << 28)
#define ST_CU1  (1u << 29)
/* Cause */
#define CA_IV   (1u << 23)
#define CA_DC   (1u << 27)
#define CA_TI   (1u << 30)
#define CA_BD   (1u << 31)
/* Debug */
#define DBG_DM  (1u << 30)

/* What an external interrupt controller (EIC mode, Config3.VEIC) gives
 * the core: the requested level (RIPL, 0 for none), the handler's offset
 * from EBASE, and the shadow set it runs in. It is told what the core
 * itself requests -- the timer's interrupt and the two software
 * interrupts -- and when the core takes its request. */
struct mips_eic {
    void (*core_irq)(void *ctx, int timer, int sw0, int sw1);
    void (*taken)(void *ctx);
    /* whether the core timer's interrupt is enabled at the controller,
     * so a wait for it can end */
    int (*timer_on)(void *ctx);
    void *ctx;
    int ripl;
    u32 offset;
    int srs;
};

struct mips_tlbe {
    u32 hi, lo0, lo1, mask;
    int g;                          /* global: both EntryLo's G */
};

struct mips_state {
    struct cpu cpu;                 /* the interface; first */
    struct sim *sim;
    int pic32;                      /* the PIC32MZ's core: EIC mode, its IDs */

    u32 gpr[MIPS_SETS][32];
    u32 *r;                         /* the set executing: gpr[SRSCtl.CSS] */
    u32 hi[4], lo[4];               /* HI and LO, and the DSP's $ac1-$ac3 */
    u32 dspctl;
    u32 pc;                         /* the instruction executing */
    u32 npc;                        /* the one after it: a branch's target */
    int slot;                       /* pc is a branch's delay slot */
    /* while an instruction executes: where execution goes after it, the
     * one after that, and whether the next is a delay slot. These three
     * addresses carry the ISA mode in bit 0 (1: microMIPS), as a jump's
     * target does; pc does not. */
    u32 nx, nnx;
    int nslot;
    int isa;                        /* executing the compressed ISA, not MIPS32 */
    int micromips;                  /* that is microMIPS (Config3.ISA) */
    int mips16;                     /* that is MIPS16e (Config1.CA) */
    int bootcfg;                    /* a PIC32's: DEVCFG0.BOOTISA at reset */
    /* the delay slot's branch (its address, the ISA bit in bit 0) for
     * EPC, and the size the slot's instruction must be (2 or 4, or 0 for
     * either: microMIPS's jals, jalr and the linking branches) */
    u32 bpc, nbpc;
    int ds, nds;
    int btaken, nbtaken;            /* the slot's branch was taken */

    /* coprocessor 0 */
    u32 status, cause, epc, errorepc, badvaddr, ebase, intctl, srsctl;
    u32 srsmap, compare, config0, hwrena, userlocal, lladdr, depc, debug;
    u32 desave, wired, index, entryhi, entrylo0, entrylo1, pagemask;
    u32 context, errctl, taglo[4], watchlo, watchhi;
    u32 prid, config1, config2, config3, config5, config7;
    struct mips_tlbe tlb[MIPS_TLB];
    int llbit;
    u32 llval;                      /* what LL read: SC compares it, as QEMU does */
    int dsp, fpu;                   /* the DSP ASE (MX), an FPU (CU1) */
    u32 core_lines;                 /* what the EIC was last told */

    /* Count: count_base at the cycle count_at, counting every other
     * cycle; it reaches Compare at ti_at */
    u32 count_base;
    u64 count_at, ti_at;
    int count_on;                   /* the core timer's device: running */

    /* the FPU (mips-fpu.c): 32 registers of 64 bits (Status.FR 1), or
     * 32 of 32 paired for a double (FR 0) in their low halves; FCSR, FIR */
    u64 fpr[32];
    u32 fcsr, fir;

    struct mips_eic *eic;           /* EIC mode's controller, or 0 */
    u32 ip_hw;                      /* compatibility mode: IP2-IP7 lines */

    /* the instruction executing raised an exception: the first wins,
     * and it writes nothing */
    int exc;
    u32 exc_code, exc_bad, exc_ce;
    int exc_bad_set, exc_refill;
    int watch_stop;                 /* a debugger's watchpoint: not run */
    u32 *exc_regs;                  /* the set the last exception interrupted */
    int changed;                    /* it changed a register or memory */
    u32 insn;
};

extern struct mips_state *ms;

int mips_is(const struct cpu *c);

/* mips.c, for mips-micro.c: a MIPS32 instruction, executed for the
 * microMIPS one it encodes the same operation as; the Coprocessor
 * Unusable exception; an instruction the part has and EmbSim does not
 * model; an unconditional branch to itself, which ends the run when
 * nothing can interrupt it (`slot_nop`: its delay slot does nothing) */
void mips_exec32(u32 i);
void mips_raise_cpu(u32 unit);
void mips_unmodelled(const char *what);
void mips_self_loop(u32 target, int slot_nop);
/* mips-micro.c: a microMIPS instruction (a 16-bit one in the low half,
 * or both halves, the first in bits 31:16), its size, its cost */
int mips_micro_size(u32 first);
void mips_micro_exec(u32 i, int size);
int mips_micro_cost(u32 i, int size);
/* mips16.c: the same for MIPS16e (an EXTENDed instruction, or jal and
 * jalx, in both halves) */
int mips16_size(u32 first);
void mips16_exec(u32 i, int size);
int mips16_cost(u32 i, int size);

/* mips.c, for mips-fpu.c: the instruction's memory accesses, its branch,
 * and its exceptions */
u32 mips_load(u32 va, int n);
void mips_store(u32 va, int n, u32 v);
void mips_branch(int taken, u32 target, int likely);
void mips_raise(u32 code);
/* mips-fpu.c: a COP1, COP1X, LWC1, LDC1, SWC1, SDC1 or MOVCI instruction,
 * with Status.CU1 set */
void mips_fp_exec(u32 i);
void mips_fp_reset(struct mips_state *k);
/* mips-dsp.c: a SPECIAL3 instruction of the DSP ASE (function 0x0c,
 * 0x10-0x13, 0x18, 0x30, 0x31, or 0x38 but rddsp and wrdsp), with the
 * DSP present and Status.MX set */
void mips_dsp_exec(u32 i);
/* an image's address, as the bus has it: kseg0 and kseg1 unmapped, the
 * rest as it is (the loader's segments, a debugger's accesses) */
u32 mips_phys(u32 va);
/* EIC mode: the controller drives the core */
void mips_eic_attach(struct sim *s, struct mips_eic *e);
/* the controller's request changed */
void mips_eic_changed(struct sim *s);

#endif
