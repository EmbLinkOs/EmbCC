/* pic24.h -- the PIC24F core's state, shared by its file and by the
 * interrupt controller that drives it (pic24-intc.c).
 *
 *   pic24.c      the core: the PIC24F instruction set (24-bit words,
 *                the W registers memory-mapped, the addressing modes,
 *                REPEAT, the iterative divide), the data space with its
 *                Extended Data Space and program space visibility, the
 *                table reads, exceptions (traps and interrupts) through
 *                the vector table, DISI, PWRSAV; step, reset, and the
 *                CPU interface
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
/* CORCON */
#define CORCON_IPL3 0x0008u

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
    /* the core raised trap n (TRAP_*) */
    void (*trap)(void *ctx, int n);
    /* INTCON1.NSTDIS: interrupts do not nest */
    int (*nstdis)(void *ctx);
    void *ctx;
};

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
    u8 traps;                       /* traps pending, by number */
    int sleeping;                   /* PWRSAV: until an interrupt */
    u32 prog_end;                   /* past the last program address */
    u64 disi_at;                    /* the cycle DISICNT was last counted at */
    struct pic24_intc *intc;
};

extern struct pic24_state *ps;

int pic24_is(const struct cpu *c);
/* the controller drives the core */
void pic24_intc_attach(struct sim *s, struct pic24_intc *ic);
/* a word of program memory (24 bits) at an even program address */
u32 pic24_prog(struct sim *s, u32 addr);
/* the core's clock (Fcy, instruction cycles per second), from the
 * configuration words and OSCCON and CLKDIV: the time the board reports */
u32 pic24_fcy(struct sim *s);

#endif
