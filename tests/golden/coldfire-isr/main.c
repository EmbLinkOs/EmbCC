/* ColdFire interrupt handlers, run: PIT0 (INTC0 source 4, vector 68 of
 * QEMU's mcf5208evb) interrupts code that keeps a pattern in d0-d7 and
 * a0-a5, hundreds of times, and a C computation -- first with a handler
 * that only counts (a leaf), then with one that calls a function doing
 * 64-bit arithmetic and a division (helper calls), which may change d0,
 * d1, a0 and a1. A handler that left one changed, or returned with rts
 * instead of rte (the format word and SR are on its stack), shows up in
 * torture's result, the computation's value, or a fault. */
#include <stdio.h>

#define PIT0_PCSR   (*(volatile unsigned short *)0xfc080000u)
#define PIT0_PMR    (*(volatile unsigned short *)0xfc080002u)
#define INTC0_IMRL  (*(volatile unsigned *)0xfc04800cu)
#define INTC0_ICR(n) (*(volatile unsigned char *)(0xfc048040u + (n)))
#define PCSR_EN     0x0001
#define PCSR_RLD    0x0002
#define PCSR_PIF    0x0004
#define PCSR_PIE    0x0008
#define PCSR_OVW    0x0010
#define PIT0_SRC    4
#define VECTORS     ((void (**)(void))0x40000000u)  /* the harness's VBR */

volatile unsigned ticks, extra;
static long long acc = 1;
static const unsigned short run = PCSR_EN | PCSR_RLD | PCSR_PIE | PCSR_OVW;

static __attribute__((noinline)) unsigned work(unsigned x)
{
    acc = acc * 7 + x;
    return (unsigned)(acc / 13) ^ x;
}

/* each one clears PIT0's flag, or the level-triggered line stays up */
void __attribute__((interrupt)) tick_leaf(void)
{
    PIT0_PCSR = run | PCSR_PIF;
    ticks++;
}

void __attribute__((interrupt_handler)) tick_calls(void)
{
    PIT0_PCSR = run | PCSR_PIF;
    ticks++;
    extra += work(ticks);
}

/* n iterations with a pattern in d0-d5 and a0-a5 and a counter pair in
 * d6/d7; returns 0, or 1 when a register changed under it */
unsigned torture(unsigned n);
__asm__(
"    .global torture\n"
"torture:\n"
"    lea -40(%sp), %sp\n"
"    movem.l %d2-%d7/%a2-%a5, (%sp)\n"
"    move.l 44(%sp), %d7\n"
"    move.l %d7, %d6\n"
"    addq.l #7, %d6\n"
"    move.l #0x11110001, %d0\n"
"    move.l #0x22220002, %d1\n"
"    move.l #0x33330003, %d2\n"
"    move.l #0x44440004, %d3\n"
"    move.l #0x55550005, %d4\n"
"    move.l #0x66660006, %d5\n"
"    movea.l #0x77770007, %a0\n"
"    movea.l #0x88880008, %a1\n"
"    movea.l #0x99990009, %a2\n"
"    movea.l #0xaaaa000a, %a3\n"
"    movea.l #0xbbbb000b, %a4\n"
"    movea.l #0xcccc000c, %a5\n"
"1:  cmpi.l #0x11110001, %d0\n"
"    bne.w 2f\n"
"    cmpi.l #0x22220002, %d1\n"
"    bne.w 2f\n"
"    cmpi.l #0x33330003, %d2\n"
"    bne.w 2f\n"
"    cmpi.l #0x44440004, %d3\n"
"    bne.w 2f\n"
"    cmpi.l #0x55550005, %d4\n"
"    bne.w 2f\n"
"    cmpi.l #0x66660006, %d5\n"
"    bne.w 2f\n"
"    cmpa.l #0x77770007, %a0\n"
"    bne.w 2f\n"
"    cmpa.l #0x88880008, %a1\n"
"    bne.w 2f\n"
"    cmpa.l #0x99990009, %a2\n"
"    bne.w 2f\n"
"    cmpa.l #0xaaaa000a, %a3\n"
"    bne.w 2f\n"
"    cmpa.l #0xbbbb000b, %a4\n"
"    bne.w 2f\n"
"    cmpa.l #0xcccc000c, %a5\n"
"    bne.w 2f\n"
"    subq.l #7, %d6\n"
"    cmp.l %d7, %d6\n"
"    bne.w 2f\n"
"    addq.l #7, %d6\n"
"    subq.l #1, %d6\n"
"    subq.l #1, %d7\n"
"    bne.w 1b\n"
"    moveq #0, %d0\n"
"    bra.w 3f\n"
"2:  moveq #1, %d0\n"
"3:  movem.l (%sp), %d2-%d7/%a2-%a5\n"
"    lea 40(%sp), %sp\n"
"    rts\n");

/* a computation whose every intermediate is in registers at -O2 */
static __attribute__((noinline)) unsigned long long mix(unsigned n)
{
    unsigned long long h = 0x9e3779b97f4a7c15ull;
    unsigned a = 1, b = 2, c = 3, d = 4, e = 5;
    for (unsigned i = 0; i < n; i++) {
        a += b ^ i; b = (b << 3) | (b >> 29); c += a * 5;
        d ^= c + e; e = e * 3 + d; h = h * 31 + (a ^ b ^ c ^ d ^ e);
    }
    return h;
}

static void sr_ipl(int level)
{
    if (level)
        __asm__ volatile ("move.w #0x2700, %sr");
    else
        __asm__ volatile ("move.w #0x2000, %sr");
}

/* one phase: the handler, and PIT0's period (the calling handler needs a
 * longer one, or it would be all the CPU did) */
static int phase(const char *name, void (*h)(void), unsigned period,
                 unsigned long long want)
{
    unsigned bad = 0, wrong = 0, n, rounds = 0;
    VECTORS[64 + PIT0_SRC] = h;
    ticks = 0;
    PIT0_PCSR = 0;
    PIT0_PMR = (unsigned short)period;
    PIT0_PCSR = run;
    sr_ipl(0);
    /* again until twenty interrupts have landed in the work: QEMU's timer
     * keeps the host's time, and on a fast runner one round can be over
     * first (rx-isr's -O2 saw sixteen). The bound ends a timer that never
     * fires. */
    do {
        bad |= torture(1000000);
        wrong |= mix(200000) != want;
    } while (ticks < 20 && ++rounds < 40);
    sr_ipl(7);
    PIT0_PCSR = 0;
    n = ticks;
    printf("%s: %u interrupts, torture %s, mix %s\n", name, n,
           bad ? "CHANGED" : "kept", wrong ? "WRONG" : "right");
    return bad || wrong || n < 20;
}

int main(void)
{
    unsigned long long want = mix(200000);      /* uninterrupted */
    int fail = 0;
    INTC0_ICR(PIT0_SRC) = 3;                     /* level 3 */
    INTC0_IMRL = ~(1u << PIT0_SRC) & ~1u;        /* and unmasked */
    fail |= phase("leaf", tick_leaf, 40, want);
    fail |= phase("calls", tick_calls, 400, want);
    return fail ? 1 : 42;
}
