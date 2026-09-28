/* Atomics on a microcontroller, run and compared with the host
 * (tests/golden/thumb-atomic.sh).
 *
 * Part one is their meaning: every operation at every width, signed and
 * not, a compare-and-swap that succeeds and one that fails.
 *
 * Part two is why they exist. On a single core an exclusive store only
 * fails when something takes the reservation away -- on a Cortex-M, any
 * exception -- so a program that never takes an interrupt never runs the
 * retry path at all. This one runs a SysTick handler that updates the
 * same variables, fast, while main updates them 200000 times; every total
 * must come out exactly iterations + ticks. A retry that does not retry,
 * or a loop that writes a stale value, loses updates and shows here. */
#include <stdatomic.h>
void writec(int c);
void puts_(const char *s);
typedef unsigned u32;
static void hx(u32 v)
{
    for (int i = 28; i >= 0; i -= 4)
        writec("0123456789abcdef"[(v >> i) & 15]);
    writec(' ');
}

static _Atomic int ai;
static _Atomic unsigned short ah;
static _Atomic signed char ac;
static int plain = 5;
static signed char sc = -3;
static unsigned short us = 0xfff0;

static void semantics(void)
{
    u32 h = 0;
    atomic_store(&ai, 10);
    h = h * 31 + (u32)atomic_fetch_add(&ai, 5);           /* 10 */
    h = h * 31 + (u32)atomic_fetch_sub(&ai, 20);          /* 15 */
    h = h * 31 + (u32)atomic_exchange(&ai, 0x5a5a);       /* -5 */
    h = h * 31 + (u32)atomic_fetch_and(&ai, 0x0ff0);
    h = h * 31 + (u32)atomic_fetch_or(&ai, 0x1001);
    h = h * 31 + (u32)atomic_fetch_xor(&ai, 0xffff);
    h = h * 31 + (u32)atomic_load(&ai);
    {
        int e = 7;
        h = h * 31 + (u32)atomic_compare_exchange_strong(&ai, &e, 99);
        h = h * 31 + (u32)e;                               /* the value seen */
        e = atomic_load(&ai);
        h = h * 31 + (u32)atomic_compare_exchange_strong(&ai, &e, 99);
        h = h * 31 + (u32)atomic_load(&ai);
    }
    /* sub-word: zero-extended loads, signed results, wrap at the width */
    atomic_store(&ac, -100);
    h = h * 31 + (u32)(int)atomic_fetch_sub(&ac, 100);     /* -100 */
    h = h * 31 + (u32)(int)atomic_load(&ac);               /* wrapped: 56 */
    h = h * 31 + (u32)(int)atomic_exchange(&ac, -1);
    {
        signed char e = -1;
        h = h * 31 + (u32)atomic_compare_exchange_strong(&ac, &e, -128);
        h = h * 31 + (u32)(int)atomic_load(&ac);
        e = 5;
        h = h * 31 + (u32)atomic_compare_exchange_strong(&ac, &e, 1);
        h = h * 31 + (u32)(int)e;
    }
    atomic_store(&ah, 0xfffe);
    h = h * 31 + (u32)atomic_fetch_add(&ah, 3);            /* 0xfffe */
    h = h * 31 + (u32)atomic_load(&ah);                    /* wrapped: 1 */
    h = h * 31 + (u32)atomic_fetch_or(&ah, 0x8000);
    /* the __sync forms, on plain objects */
    h = h * 31 + (u32)__sync_fetch_and_add(&plain, 3);
    h = h * 31 + (u32)__sync_val_compare_and_swap(&plain, 8, 40);
    h = h * 31 + (u32)__sync_val_compare_and_swap(&plain, 8, 41);  /* fails */
    h = h * 31 + (u32)__sync_bool_compare_and_swap(&plain, 40, 42);
    h = h * 31 + (u32)__sync_fetch_and_nand(&plain, 0xf0);
    h = h * 31 + (u32)plain;
    h = h * 31 + (u32)(int)__sync_fetch_and_add(&sc, -1);
    h = h * 31 + (u32)(int)sc;
    h = h * 31 + (u32)__sync_val_compare_and_swap(&us, 0xfff0, 0x1234);
    h = h * 31 + (u32)us;
    __sync_synchronize();
    hx(h);
}

#if defined(__arm__)
#define VTOR   (*(volatile u32 *)0xE000ED08u)
#define STCTRL (*(volatile u32 *)0xE000E010u)
#define STLOAD (*(volatile u32 *)0xE000E014u)
#define STVAL  (*(volatile u32 *)0xE000E018u)
static _Atomic u32 cnt_add, cnt_cas;
static _Atomic unsigned short cnt_h;
static _Atomic unsigned char cnt_b;
static volatile u32 ticks;
static void (*vt[64])(void) __attribute__((aligned(256)));
static void tick(void)
{
    atomic_fetch_add(&cnt_add, 1);
    u32 o = atomic_load(&cnt_cas);
    while (!atomic_compare_exchange_weak(&cnt_cas, &o, o + 1))
        ;
    atomic_fetch_add(&cnt_h, 1);
    atomic_fetch_add(&cnt_b, 1);
    ticks++;
}
static void contention(void)
{
    const u32 n = 200000;
    for (int k = 0; k < 64; k++) vt[k] = ((void (**)(void))VTOR)[k];
    vt[15] = tick;
    VTOR = (u32)vt;
    STLOAD = 97;                 /* a tick every few dozen instructions */
    STVAL = 0;
    STCTRL = 7;                  /* enable, interrupt, core clock */
    for (u32 k = 0; k < n; k++) {
        atomic_fetch_add(&cnt_add, 1);
        u32 o = atomic_load(&cnt_cas);
        while (!atomic_compare_exchange_weak(&cnt_cas, &o, o + 1))
            ;
        atomic_fetch_add(&cnt_h, 1);
        atomic_fetch_add(&cnt_b, 1);
    }
    STCTRL = 0;
    u32 t = ticks;
    hx(t > 100);                                         /* it interleaved */
    hx(atomic_load(&cnt_add) == n + t);
    hx(atomic_load(&cnt_cas) == n + t);
    hx(atomic_load(&cnt_h) == (unsigned short)(n + t));
    hx(atomic_load(&cnt_b) == (unsigned char)(n + t));
}
#else
static void contention(void) { hx(1); hx(1); hx(1); hx(1); hx(1); }
#endif

int main(void)
{
    semantics();
    contention();
    puts_("\n==END==\n");
    return 0;
}
