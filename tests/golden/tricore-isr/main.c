/* TriCore interrupt handlers, run: QEMU's tricore_testboard has no
 * interrupt router, but a trap enters a handler the same way an interrupt
 * does -- the upper context (D8-D15, A10-A15) saved by the hardware, the
 * lower one (D0-D7, A2-A7, A11) the interrupted code's still -- and leaves
 * it the same way, by rfe. So a loop that keeps a pattern in D0-D7 and
 * A2-A7 raises a syscall trap (class 6) over and over, and the trap's
 * vector jumps to a handler compiled with __attribute__((interrupt)):
 * first one that only counts, then one that calls a function doing 64-bit
 * arithmetic and a division, which changes the lower context freely. A
 * handler without svlcx/rslcx shows up as a changed register; one that
 * returned with ret instead of rfe would never come back to the loop. */
#include <stdio.h>

volatile unsigned traps, extra;
static long long acc = 1;

static __attribute__((noinline)) unsigned work(unsigned x)
{
    acc = acc * 7 + x;
    return (unsigned)(acc / 13) ^ x;
}

void __attribute__((interrupt)) trap_leaf(void) { traps++; }
void __attribute__((interrupt)) trap_calls(void) { traps++; extra += work(traps); }

/* the harness's report, for every other class */
void harness_trap(int cls, int tin, unsigned pc);

/* BTV's table: eight 32-byte vectors */
static unsigned trap_tab[64] __attribute__((aligned(256)));

/* movh.a a15, hi ; lea a15, [a15]lo ; ji a15 -- a15 is the upper
 * context's, which the trap saved */
static void vector_to(unsigned *v, unsigned h)
{
    unsigned hi = ((h + 0x8000u) >> 16) & 0xffffu, lo = h & 0xffffu;
    v[3] = 0xf0000091u | (hi << 12);
    v[4] = 0x0000ffd9u | ((lo & 0x3fu) << 16) |
           (((lo >> 10) & 0x3fu) << 22) | (((lo >> 6) & 0xfu) << 28);
    v[5] = 0x00300f2du;
}

static void install(void (*h)(void))
{
    for (unsigned c = 0; c < 8; c++) {
        unsigned *v = trap_tab + 8 * c;
        v[0] = v[1] = v[2] = 0;                         /* nops */
        if (c == 6) {
            vector_to(v, (unsigned)h);
        } else {
            v[0] = 0x4000003bu | (c << 12);             /* mov d4, class */
            v[1] = 0x51f0f00bu;                         /* mov d5, d15 */
            v[2] = 0x64c0b001u;                         /* mov.d d6, a11 */
            vector_to(v, (unsigned)harness_trap);
        }
    }
    __asm__ volatile("dsync\n isync\n mtcr btv, %0\n isync"
                     : : "d"(trap_tab) : "memory");
}

/* d4 traps with a pattern in D0-D7 and A2-A7, checked after each;
 * 0, or 1 when one changed */
unsigned torture(unsigned n);
__asm__(
"        .global torture\n"
"        .type   torture, @function\n"
"torture:\n"
"        mov     d14, d4\n"
"        mov     d0, 1\n"
"        mov     d1, 2\n"
"        mov     d2, 3\n"
"        mov     d3, 4\n"
"        mov     d4, 5\n"
"        mov     d5, 6\n"
"        mov     d6, 7\n"
"        mov     d7, -1\n"
"        mov     d15, 2\n"
"        mov.a   a2, d15\n"
"        mov     d15, 3\n"
"        mov.a   a3, d15\n"
"        mov     d15, 4\n"
"        mov.a   a4, d15\n"
"        mov     d15, 5\n"
"        mov.a   a5, d15\n"
"        mov     d15, 6\n"
"        mov.a   a6, d15\n"
"        mov     d15, 7\n"
"        mov.a   a7, d15\n"
"1:      syscall 0\n"
"        jne     d0, 1, 9f\n"
"        jne     d1, 2, 9f\n"
"        jne     d2, 3, 9f\n"
"        jne     d3, 4, 9f\n"
"        jne     d4, 5, 9f\n"
"        jne     d5, 6, 9f\n"
"        jne     d6, 7, 9f\n"
"        jne     d7, -1, 9f\n"
"        mov.d   d15, a2\n"
"        jne     d15, 2, 9f\n"
"        mov.d   d15, a3\n"
"        jne     d15, 3, 9f\n"
"        mov.d   d15, a4\n"
"        jne     d15, 4, 9f\n"
"        mov.d   d15, a5\n"
"        jne     d15, 5, 9f\n"
"        mov.d   d15, a6\n"
"        jne     d15, 6, 9f\n"
"        mov.d   d15, a7\n"
"        jne     d15, 7, 9f\n"
"        addi    d14, d14, -1\n"
"        jnz     d14, 1b\n"
"        mov     d2, 0\n"
"        ret\n"
"9:      mov     d2, 1\n"
"        ret\n");

static int phase(const char *name, void (*h)(void))
{
    unsigned bad;
    install(h);
    traps = 0;
    bad = torture(2000);
    printf("%s: %u traps, torture %s\n", name, traps, bad ? "CHANGED" : "kept");
    return bad || traps != 2000;
}

int main(void)
{
    int fail = 0;
    fail |= phase("leaf", trap_leaf);
    fail |= phase("calls", trap_calls);
    return fail ? 1 : 42;
}
