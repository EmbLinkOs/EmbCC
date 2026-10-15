/* pic24.h -- the PIC24F and dsPIC33E core's state, shared by its files
 * and by the interrupt controller that drives it (pic24-intc.c).
 *
 *   pic24.c      the core: the PIC24F instruction set (24-bit words,
 *                the W registers memory-mapped, the addressing modes,
 *                REPEAT, the iterative divide), the data space with its
 *                Extended Data Space and program space visibility, the
 *                table reads, exceptions (traps and interrupts) through
 *                the vector table, DISI, PWRSAV; step, reset, and the
 *                CPU interface; a dsPIC33E's: the PIC24E's instructions
 *                and timing, the DO loops, DIVF
 *   pic24-dsp.c  a dsPIC33E's DSP engine (the accumulators, the MAC
 *                class with its prefetches, the saturation and rounding
 *                logic) and its address generators' modulo and
 *                bit-reversed addressing
 *
 * The core executing is `ps`, set by each entry point of the CPU
 * interface. The bus holds both of the part's spaces:
 *   - program memory at 0: the word at program address P (P even) is the
 *     four bytes at 2P -- its low, middle and high bytes and a phantom 0,
 *     as an XC16 image lays them out, so an ELF segment's bytes go to
 *     twice its load address (pic24_phys);
 *   - the data space at PIC24_DATA, by its 24-bit Extended Data Space
 *     address: the SFRs and RAM below 0x8000, EDS page 1 (RAM above
 *     0x8000) at 0x8000, the EPMP's pages above. */
#ifndef EMBSIM_PIC24_H
#define EMBSIM_PIC24_H

#include "sim.h"

#define PIC24_DATA 0x2000000u
/* the configuration memory's program addresses start here */
#define PIC24_CONFIG 0x800000u

/* SR */
#define SR_C    0x0001u
#define SR_Z    0x0002u
#define SR_OV   0x0004u
#define SR_N    0x0008u
#define SR_RA   0x0010u
#define SR_IPL  0x00e0u
#define SR_DC   0x0100u
/* a dsPIC's: the DO loop's, the DSP engine's */
#define SR_DA   0x0200u
#define SR_SAB  0x0400u
#define SR_OAB  0x0800u
#define SR_SB   0x1000u
#define SR_SA   0x2000u
#define SR_OB   0x4000u
#define SR_OA   0x8000u
/* CORCON */
#define CORCON_IF     0x0001u
#define CORCON_RND    0x0002u
#define CORCON_SFA    0x0004u       /* the PIC24E's and dsPIC33E's */
#define CORCON_IPL3   0x0008u
#define CORCON_ACCSAT 0x0010u
#define CORCON_SATDW  0x0020u
#define CORCON_SATB   0x0040u
#define CORCON_SATA   0x0080u
#define CORCON_DL     0x0700u
#define CORCON_EDT    0x0800u
#define CORCON_US     0x3000u
#define CORCON_VAR    0x8000u

/* the traps, by their vector (0x000004 + 2n) */
enum {
    TRAP_OSC, TRAP_ADDR, TRAP_HARD, TRAP_STACK, TRAP_MATH, TRAP_RES5,
    TRAP_SOFT, TRAP_RES7
};

/* What the interrupt controller gives the core: the request it presents
 * (the highest priority enabled, then the lowest number), and what the
 * core tells it -- that it took one, and of the traps it raises, which
 * set INTCON1's status bits. */
struct pic24_intc {
    /* the priority (1 to 7) of the request presented, and its interrupt
     * number; 0 when there is none */
    int (*request)(void *ctx, int *irq);
    /* the core took interrupt irq, now at priority ipl */
    void (*taken)(void *ctx, int irq, int ipl);
    /* a condition of the math error trap's (PIC24_MATH_*): whether
     * INTCON1 makes it one (OVATE, OVBTE, COVTE; the others always),
     * its status bit set (OVAERR...) when it does */
    int (*math)(void *ctx, int why);
    /* the core raised trap n (TRAP_*) */
    void (*trap)(void *ctx, int n);
    /* INTCON1.NSTDIS: interrupts do not nest */
    int (*nstdis)(void *ctx);
    void *ctx;
};

/* What the flash controller (pic24-nvm.c) gives the core: its write
 * latches, which TBLWTL and TBLWTH write and the table reads read at
 * 0xFA0000 (TBLPAG 0xFA) */
struct pic24_nvm {
    u32 (*latch_read)(void *ctx, u32 addr);
    void (*latch_write)(void *ctx, u32 addr, int high, int n, u16 v);
    void *ctx;
};

#define PIC24_LATCHES 0xfa0000u

/* the math error trap's conditions */
enum { PIC24_MATH_DIV0, PIC24_MATH_OVA, PIC24_MATH_OVB, PIC24_MATH_COVA,
       PIC24_MATH_COVB, PIC24_MATH_SFTAC };

/* the DO loops' stack (a dsPIC33E's): the loop's registers each DO
 * saves, four loops deep; a fifth DO is the soft trap (INTCON3.DOOVR) */
#define PIC24_DO_LEVELS 4

struct pic24_state {
    struct cpu cpu;                 /* the interface; first */
    struct sim *sim;

    u16 w[16];
    u16 winit;                      /* the W registers written since reset */
    u16 splim, sr, corcon, dsrpag, dswpag, rcount, tblpag, disicnt;
    int splim_set;                  /* SPLIM written since reset */
    u32 pc;                         /* the instruction executing */
    u32 npc;                        /* where execution goes after it */
    u16 sw[4], ssr;                 /* PUSH.S's shadows: W0-W3, SR's flags */
    u32 insn;
    int size;                       /* its words */
    int changed;                    /* it wrote a register or memory */
    int extra;                      /* cycles it takes beyond one */
    int trap;                       /* the trap it raised (TRAP_* + 1), or 0 */
    u32 trap_pc;                    /* the last trap's instruction */
    char trap_why[96];              /* and what it did */
    u8 traps;                       /* traps pending, by number */
    int sleeping;                   /* PWRSAV: until an interrupt */
    u32 prog_end;                   /* past the last program address */
    u64 disi_at;                    /* the cycle DISICNT was last counted at */
    struct pic24_intc *intc;
    struct pic24_nvm *nvm;
    /* NVMKEY's unlock: 0x55 then 0xAA (key_step 2) at instruction key_at */
    int key_step;
    u64 key_at;
    /* the family: the PIC24E's and dsPIC33E's instructions and timing
     * (e), and a dsPIC's DSP engine (dsp) */
    int e, dsp;
    u16 ram_base;                   /* the RAM's start: W15 out of reset */
    /* the DSP engine's accumulators (40 bits, kept sign-extended) */
    s64 acc[2];
    /* the DO loop: its count, first and last instructions, the levels
     * nested (CORCON.DL) saved; EDT written: it ends at this pass's end */
    u16 dcount;
    u32 dostart, doend;
    struct { u16 dcount; u32 dostart, doend; } dostack[PIC24_DO_LEVELS];
    int edt;
    /* the address generators: MODCON, the modulo buffers, XBREV; the Y
     * data space [y_lo, y_hi) */
    u16 modcon, xmodsrt, xmodend, ymodsrt, ymodend, xbrev, mstrpr;
    u32 y_lo, y_hi;
};

extern struct pic24_state *ps;

int pic24_is(const struct cpu *c);
void pic24_nvm_attach(struct sim *s, struct pic24_nvm *n);
/* NVMKEY written (the flash controller tells the core), and whether the
 * unlock it makes is open: 0x55 and 0xAA, the instruction just before
 * (or the one before that, as Microchip's sequences allow) */
void pic24_nvmkey(struct sim *s, u16 v);
int pic24_unlocked(struct sim *s);
/* a trap's name ("an address error"), by its number (TRAP_*) */
const char *pic24_trap_name(int n);
/* the controller drives the core */
void pic24_intc_attach(struct sim *s, struct pic24_intc *ic);
/* a word of program memory (24 bits) at an even program address */
u32 pic24_prog(struct sim *s, u32 addr);
/* the core's clock (Fcy, instruction cycles per second), from the
 * configuration words and OSCCON and CLKDIV: the time the board reports */
u32 pic24_fcy(struct sim *s);
/* the oscillator's state out of reset, from the configuration words
 * (pic24-osc.c): the core's reset calls it, the image being loaded */
void pic24_osc_power_on(struct sim *s);

/* ---- the core's own, between pic24.c and pic24-dsp.c (on `ps`) ---------- */

/* a read or write of n bytes at data address ea; -1 with a trap raised */
int pic24_core_read(u32 ea, int n, u16 *v);
int pic24_core_write(u32 ea, int n, u16 v);
void pic24_core_set_w(int n, u16 v);
/* W n as a pointer: 1 if it has been written, else the run ends */
int pic24_core_pointer(int n);
/* an operand of mode m (ppp/qqq: Ws, [Ws], [Ws--], [Ws++], [--Ws],
 * [++Ws], [Ws+Wb]), its register modified; -1 when it cannot be */
int pic24_core_src(int mode, int r, int wb, int n, u16 *v);
int pic24_core_dst(int mode, int r, int wb, int n, u16 v);
int pic24_core_bad_opcode(void);
/* the instruction raises trap n (TRAP_*), as `why` says */
int pic24_core_trap(int n, const char *why);
/* a math error trap's condition (PIC24_MATH_*): the trap if INTCON1 says */
void pic24_core_math(int why);

/* pic24-dsp.c: the DSP instructions (opcodes 0xC0-0xCF, 0xF0-0xF3), the
 * accumulators' result logic, the MCU multiplies into an accumulator */
int pic24_dsp_exec(u32 i);
void pic24_dsp_mul_acc(int a, s64 product);
/* the address generators: W r's value w modified by d (+-1, 2, 4, 6:
 * through the X AGU, or the Y AGU's with y), modulo or bit-reversed as
 * MODCON and XBREV say; an offset address [Wr+offset] corrected */
u16 pic24_agu_step(int r, u16 w, int d, int write, int y);
u16 pic24_agu_offset(int r, u16 ea, int neg, int y);
/* the accumulators' registers (0x22-0x2C), read and written */
u16 pic24_acc_read(u32 a);
void pic24_acc_write(u32 a, u16 v);

#endif
