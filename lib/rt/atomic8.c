/* Eight-byte atomics on the 32-bit targets: libatomic's sized entry
 * points, which the compiler calls for every eight-byte atomic there
 * (src/ir/irgen.c, target_atomic8_libcall) -- as GCC and clang do, so an
 * object of theirs links against this archive too.
 *
 *   u64  __atomic_load_8(const volatile void *p, int order)
 *   void __atomic_store_8(volatile void *p, u64 v, int order)
 *   u64  __atomic_exchange_8(volatile void *p, u64 v, int order)
 *   bool __atomic_compare_exchange_8(volatile void *p, void *expected,
 *                                    u64 desired, int success, int failure)
 *   u64  __atomic_fetch_{add,sub,and,or,xor,nand}_8(volatile void *p,
 *                                                  u64 v, int order)
 *   u64  __atomic_{add,sub,and,or,xor,nand}_fetch_8(...)  (GCC calls these)
 *   and libatomic's generic __atomic_load, __atomic_store,
 *   __atomic_exchange and __atomic_compare_exchange, which take the size
 *   first (clang calls these for an _Atomic long long or double)
 *
 * Each is atomic by MASKING INTERRUPTS on the core it runs on: the mask
 * is saved, interrupts are turned off, the eight bytes are read and
 * written, and the mask is put back as it was -- so these nest inside a
 * critical section and an interrupt handler, and never turn interrupts
 * on. On one core with no other bus master that is all the atomicity
 * there is. It is NOT atomic against a second core or a DMA engine, and
 * the instructions that mask interrupts are privileged: machine mode on
 * RISC-V, supervisor mode on SPARC, PowerPC, ColdFire and RX, a
 * privileged thread or handler on Cortex-M. That is why every routine is
 * WEAK: a multi-core part's SDK (the RP2040's defines these with its
 * hardware spinlocks) or an RTOS whose tasks run unprivileged replaces
 * them by defining its own. The memory order is accepted and ignored:
 * with interrupts masked every order is sequentially consistent.
 *
 * Compiled to nothing where pointers are not four bytes: tools/build-rt.sh
 * compiles every lib/rt file for every triple, and AVR does its own
 * atomics inline.
 */
#if __SIZEOF_POINTER__ == 4

#define WEAK __attribute__((weak))

typedef unsigned int u32;
typedef unsigned long long u64;

/* ---- the interrupt mask, per architecture ------------------------------ */

#if defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'M'
/* PRIMASK: 1 masks every interrupt of configurable priority */
static u32 irq_off(void)
{
    u32 m;
    __asm__ volatile ("mrs %0, primask" : "=r"(m));
    __asm__ volatile ("cpsid i" ::: "memory");
    return m;
}
static void irq_restore(u32 m)
{
    __asm__ volatile ("msr primask, %0" : : "r"(m) : "memory");
}

#elif defined(__arm__) || defined(__ARM_ARCH_PROFILE)
/* ARMv7-A: CPSR.I; only the control field is written back */
static u32 irq_off(void)
{
    u32 m;
    __asm__ volatile ("mrs %0, cpsr" : "=r"(m));
    __asm__ volatile ("cpsid i" ::: "memory");
    return m;
}
static void irq_restore(u32 m)
{
    __asm__ volatile ("msr cpsr_c, %0" : : "r"(m) : "memory");
}

#elif defined(__riscv)
/* mstatus.MIE (bit 3), cleared and read in one csrrci; set back only if
 * it was set */
static u32 irq_off(void)
{
    u32 m;
    __asm__ volatile ("csrrci %0, mstatus, 8" : "=r"(m) : : "memory");
    return m & 8;
}
static void irq_restore(u32 m)
{
    __asm__ volatile ("csrs mstatus, %0" : : "r"(m) : "memory");
}

#elif defined(__mips)
/* MIPS32r2: di returns Status as it was and clears IE; only IE (bit 0)
 * is put back, with ei */
static u32 irq_off(void)
{
    u32 m;
    __asm__ volatile ("di %0\n\tehb" : "=r"(m) : : "memory");
    return m & 1;
}
static void irq_restore(u32 m)
{
    if (m)
        __asm__ volatile ("ei\n\tehb" ::: "memory");
}

#elif defined(__TRICORE__)
/* ICR.IE (bit 15): disable, and enable again only if it was set */
static u32 irq_off(void)
{
    u32 m;
    __asm__ volatile ("mfcr %0, 0xfe2c" : "=d"(m));
    __asm__ volatile ("disable" ::: "memory");
    return m & 0x8000u;
}
static void irq_restore(u32 m)
{
    if (m)
        __asm__ volatile ("enable" ::: "memory");
}

#elif defined(__sparc__) || defined(__PPC__) || defined(__mcoldfire__) || \
      defined(__m68k__) || defined(__XTENSA__) || defined(__RX__)
/* No inline assembler for these yet: the two routines are machine code
 * (atomic8.h), placed in .text and called through a pointer.
 *   SPARC     PSR.PIL (bits 11:8) at 15; only PIL put back
 *   PowerPC   MSR[EE] (0x8000) clear; only EE put back
 *   ColdFire  SR's interrupt mask (bits 10:8) at 7; only the mask back
 *   Xtensa    PS.INTLEVEL at 15 (rsil); PS put back whole
 *   RX        PSW.I (bit 16) clear; set again only if it was set */
#include "atomic8.h"
#define CODE __attribute__((section(".text"), aligned(4)))
#if defined(__sparc__)
CODE static const u32 off_code[] = { RT_SPARC_OFF };
CODE static const u32 on_code[] = { RT_SPARC_ON };
#elif defined(__PPC__)
CODE static const u32 off_code[] = { RT_PPC_OFF };
CODE static const u32 on_code[] = { RT_PPC_ON };
#elif defined(__XTENSA__)
CODE static const unsigned char off_code[] = { RT_XT_OFF };
CODE static const unsigned char on_code[] = { RT_XT_ON };
#elif defined(__RX__)
CODE static const unsigned char off_code[] = { RT_RX_OFF };
CODE static const unsigned char on_code[] = { RT_RX_ON };
#else
CODE static const unsigned short off_code[] = { RT_CF_OFF };
CODE static const unsigned short on_code[] = { RT_CF_ON };
#endif
typedef u32 (*off_fn)(void);
typedef void (*on_fn)(u32);
static u32 irq_off(void)
{
    u32 m = ((off_fn)(const void *)off_code)();
#if defined(__RX__)
    m &= 0x10000u;
#endif
    return m;
}
static void irq_restore(u32 m)
{
#if defined(__RX__)
    if (!m)
        return;
#endif
    ((on_fn)(const void *)on_code)(m);
}

#else
#error "lib/rt/atomic8.c: no way to mask interrupts on this target"
#endif

/* ---- the operations ------------------------------------------------------ */

#define FETCH(NAME, EXPR)                                                   \
    WEAK u64 __atomic_fetch_##NAME##_8(volatile void *p, u64 v, int order)  \
    {                                                                       \
        volatile u64 *q = (volatile u64 *)p;                                \
        u32 m = irq_off();                                                  \
        u64 old = *q;                                                       \
        (void)order;                                                        \
        *q = (EXPR);                                                        \
        irq_restore(m);                                                     \
        return old;                                                         \
    }                                                                       \
    WEAK u64 __atomic_##NAME##_fetch_8(volatile void *p, u64 v, int order)  \
    {                                                                       \
        volatile u64 *q = (volatile u64 *)p;                                \
        u32 m = irq_off();                                                  \
        u64 old = *q, nv = (EXPR);                                          \
        (void)order;                                                        \
        *q = nv;                                                            \
        irq_restore(m);                                                     \
        return nv;                                                          \
    }

FETCH(add, old + v)
FETCH(sub, old - v)
FETCH(and, old & v)
FETCH(or, old | v)
FETCH(xor, old ^ v)
FETCH(nand, ~(old & v))

WEAK u64 __atomic_load_8(const volatile void *p, int order)
{
    const volatile u64 *q = (const volatile u64 *)p;
    u32 m = irq_off();
    u64 v = *q;
    (void)order;
    irq_restore(m);
    return v;
}

WEAK void __atomic_store_8(volatile void *p, u64 v, int order)
{
    volatile u64 *q = (volatile u64 *)p;
    u32 m = irq_off();
    (void)order;
    *q = v;
    irq_restore(m);
}

WEAK u64 __atomic_exchange_8(volatile void *p, u64 v, int order)
{
    volatile u64 *q = (volatile u64 *)p;
    u32 m = irq_off();
    u64 old = *q;
    (void)order;
    *q = v;
    irq_restore(m);
    return old;
}

/* libatomic's form: no `weak` argument, and *expected written only on a
 * mismatch */
WEAK _Bool __atomic_compare_exchange_8(volatile void *p, void *expected,
                                       u64 desired, int success, int failure)
{
    volatile u64 *q = (volatile u64 *)p;
    u64 *e = (u64 *)expected;
    u32 m = irq_off();
    u64 old = *q;
    _Bool ok = old == *e;
    (void)success;
    (void)failure;
    if (ok)
        *q = desired;
    irq_restore(m);
    if (!ok)
        *e = old;
    return ok;
}

/* ---- libatomic's generic forms ------------------------------------------
 *
 * clang calls these, which take the object's size, for an _Atomic object
 * it does not count as lock-free -- an _Atomic long long or double on
 * these targets -- so an object of clang's links here too. Any size, a
 * byte at a time through volatile pointers (which no optimizer turns into
 * a call). Their names are builtins to the compiler, so they are defined
 * under internal ones and named by alias. */
typedef __SIZE_TYPE__ rt_size;
typedef volatile unsigned char vbyte;

static void gen_load(rt_size n, const volatile void *p, void *ret, int order)
{
    const vbyte *s = (const vbyte *)p;
    vbyte *d = (vbyte *)ret;
    u32 m = irq_off();
    (void)order;
    for (rt_size k = 0; k < n; k++)
        d[k] = s[k];
    irq_restore(m);
}

static void gen_store(rt_size n, volatile void *p, void *val, int order)
{
    vbyte *d = (vbyte *)p;
    const vbyte *s = (const vbyte *)val;
    u32 m = irq_off();
    (void)order;
    for (rt_size k = 0; k < n; k++)
        d[k] = s[k];
    irq_restore(m);
}

/* byte by byte, so `ret` may be `val` */
static void gen_exchange(rt_size n, volatile void *p, void *val, void *ret,
                         int order)
{
    vbyte *q = (vbyte *)p, *v = (vbyte *)val, *r = (vbyte *)ret;
    u32 m = irq_off();
    (void)order;
    for (rt_size k = 0; k < n; k++) {
        unsigned char t = q[k];
        q[k] = v[k];
        r[k] = t;
    }
    irq_restore(m);
}

static _Bool gen_cmpxchg(rt_size n, volatile void *p, void *expected,
                         void *desired, int success, int failure)
{
    vbyte *q = (vbyte *)p, *e = (vbyte *)expected, *d = (vbyte *)desired;
    _Bool ok = 1;
    u32 m = irq_off();
    (void)success;
    (void)failure;
    for (rt_size k = 0; k < n; k++)
        if (q[k] != e[k])
            ok = 0;
    for (rt_size k = 0; k < n; k++) {
        if (ok)
            q[k] = d[k];
        else
            e[k] = q[k];
    }
    irq_restore(m);
    return ok;
}

void __atomic_load(rt_size n, const volatile void *p, void *ret, int order)
    __attribute__((weak, alias("gen_load")));
void __atomic_store(rt_size n, volatile void *p, void *val, int order)
    __attribute__((weak, alias("gen_store")));
void __atomic_exchange(rt_size n, volatile void *p, void *val, void *ret,
                       int order) __attribute__((weak, alias("gen_exchange")));
_Bool __atomic_compare_exchange(rt_size n, volatile void *p, void *expected,
                                void *desired, int success, int failure)
    __attribute__((weak, alias("gen_cmpxchg")));

#endif /* __SIZEOF_POINTER__ == 4 */
