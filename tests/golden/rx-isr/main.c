/* RX interrupt handlers, run: CMT0's compare match (vector 28 of QEMU's
 * gdbsim-r5f562n8) interrupts code that keeps a pattern in every register
 * but the stack pointer, hundreds of times, and a C computation -- first
 * with a handler that only counts (a leaf), then with one that calls a
 * function doing 64-bit arithmetic and a division (a helper call), which
 * may change any caller-saved register. A handler that left one changed,
 * or returned with rts instead of rte (PSW and PC are on its stack), shows
 * up in torture's result, the computation's value, or a fault. */
#include <stdio.h>

#define ICU_IER(n)  (*(volatile unsigned char *)(0x00087200u + (n)))
#define ICU_IPR(n)  (*(volatile unsigned char *)(0x00087300u + (n)))
#define CMSTR0      (*(volatile unsigned short *)0x00088000u)
#define CMT0_CMCR   (*(volatile unsigned short *)0x00088002u)
#define CMT0_CMCOR  (*(volatile unsigned short *)0x00088006u)
#define CMI0        28

/* the relocatable vector table INTB points at */
static void (*vectors[256])(void) __attribute__((aligned(4)));

volatile unsigned ticks, extra;
static long long acc = 1;

static __attribute__((noinline)) unsigned work(unsigned x)
{
    acc = acc * 7 + x;
    return (unsigned)(acc / 13) ^ x;
}

void __attribute__((interrupt)) tick_leaf(void) { ticks++; }
void __attribute__((interrupt)) tick_calls(void) { ticks++; extra += work(ticks); }

/* r1 iterations with a pattern in r1-r13 and a counter pair in r14/r15;
 * returns 0, or 1 when a register changed under it */
unsigned torture(unsigned n);
__asm__(
"    .global _torture\n"
"_torture:\n"
"    pushm r6-r13\n"
"    mov.l r1, r14\n"
"    mov.l r1, r15\n"
"    add #7, r15\n"
"    mov.l #0x11110001, r1\n"
"    mov.l #0x22220002, r2\n"
"    mov.l #0x33330003, r3\n"
"    mov.l #0x44440004, r4\n"
"    mov.l #0x55550005, r5\n"
"    mov.l #0x66660006, r6\n"
"    mov.l #0x77770007, r7\n"
"    mov.l #0x88880008, r8\n"
"    mov.l #0x99990009, r9\n"
"    mov.l #0xaaaa000a, r10\n"
"    mov.l #0xbbbb000b, r11\n"
"    mov.l #0xcccc000c, r12\n"
"    mov.l #0xdddd000d, r13\n"
"1:  cmp #0x11110001, r1\n"
"    bne 2f\n"
"    cmp #0x22220002, r2\n"
"    bne 2f\n"
"    cmp #0x33330003, r3\n"
"    bne 2f\n"
"    cmp #0x44440004, r4\n"
"    bne 2f\n"
"    cmp #0x55550005, r5\n"
"    bne 2f\n"
"    cmp #0x66660006, r6\n"
"    bne 2f\n"
"    cmp #0x77770007, r7\n"
"    bne 2f\n"
"    cmp #0x88880008, r8\n"
"    bne 2f\n"
"    cmp #0x99990009, r9\n"
"    bne 2f\n"
"    cmp #0xaaaa000a, r10\n"
"    bne 2f\n"
"    cmp #0xbbbb000b, r11\n"
"    bne 2f\n"
"    cmp #0xcccc000c, r12\n"
"    bne 2f\n"
"    cmp #0xdddd000d, r13\n"
"    bne 2f\n"
"    add #-7, r15\n"
"    cmp r14, r15\n"
"    bne 2f\n"
"    add #7, r15\n"
"    sub #1, r15\n"
"    sub #1, r14\n"
"    bne 1b\n"
"    mov.l #0, r1\n"
"    bra 3f\n"
"2:  mov.l #1, r1\n"
"3:  popm r6-r13\n"
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

/* one phase: the handler, and the timer's period (the calling handler
 * needs a longer one, or it would be all the CPU did) */
static int phase(const char *name, void (*h)(void), unsigned period,
                 unsigned long long want)
{
    unsigned bad = 0, wrong = 0, n, rounds = 0;
    vectors[CMI0] = h;
    ticks = 0;
    CMT0_CMCOR = (unsigned short)period;
    __asm__ volatile ("setpsw i");
    /* again until twenty interrupts have landed in the work: QEMU's CMT
     * keeps the host's time, and one round of -O2 code was over after
     * sixteen on a fast runner. The bound ends a timer that never fires. */
    do {
        bad |= torture(1500000);
        wrong |= mix(400000) != want;
    } while (ticks < 20 && ++rounds < 40);
    __asm__ volatile ("clrpsw i");
    n = ticks;
    printf("%s: %u interrupts, torture %s, mix %s\n", name, n,
           bad ? "CHANGED" : "kept", wrong ? "WRONG" : "right");
    return bad || wrong || n < 20;
}

int main(void)
{
    unsigned long long want = mix(400000);      /* uninterrupted */
    int fail = 0;
    __asm__ volatile ("mvtc %0, intb" : : "r"(vectors));
    ICU_IPR(4) = 1;                              /* CMI0's priority */
    ICU_IER(CMI0 / 8) |= 1u << (CMI0 % 8);
    CMT0_CMCR = 0x40;                            /* CMIE, PCLK/8 */
    CMSTR0 = 1;
    fail |= phase("leaf", tick_leaf, 12, want);
    fail |= phase("calls", tick_calls, 50, want);
    CMSTR0 = 0;
    return fail ? 1 : 42;
}
