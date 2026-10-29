/* Atomics on RISC-V without the A extension -- an ESP32-C3's rv32imc --
 * which has no read-modify-write instruction at all: the compiler calls
 * libatomic's sized entry points for every atomic exchange, compare-
 * exchange and fetch-and-op there (src/arch/target.c,
 * target_atomic_rmw_libcall), as GCC and clang do, so an object of theirs
 * built for such a part links against these too.
 *
 *   uN   __atomic_load_N(const volatile void *p, int order)
 *   void __atomic_store_N(volatile void *p, uN v, int order)
 *   uN   __atomic_exchange_N(volatile void *p, uN v, int order)
 *   bool __atomic_compare_exchange_N(volatile void *p, void *expected,
 *                                    uN desired, int success, int failure)
 *   uN   __atomic_fetch_{add,sub,and,or,xor,nand}_N(volatile void *p,
 *                                                  uN v, int order)
 *   uN   __atomic_{add,sub,and,or,xor,nand}_fetch_N(...)   (GCC's)
 *
 * for N 1, 2 and 4, and 8 on RV64 (RV32's eight-byte ones are
 * lib/rt/atomic8.c's). Each is atomic as atomic8.c's are, by MASKING
 * INTERRUPTS: mstatus.MIE cleared and read in one csrrci, set back only if
 * it was set, so they nest. On one core with no other bus master that is
 * all the atomicity there is -- which is the part these are for. It needs
 * machine mode, and it is not atomic against a second core or DMA: every
 * routine is WEAK, so an SDK or RTOS with another way replaces them. The
 * memory order is accepted and ignored; masked, every order is
 * sequentially consistent.
 *
 * Built into every RISC-V runtime: one with the A extension never calls
 * them, and a section per function leaves them out of its images. */
#if defined(__riscv)

#define WEAK __attribute__((weak))

static unsigned long irq_off(void)
{
    unsigned long m;
    __asm__ volatile ("csrrci %0, mstatus, 8" : "=r"(m) : : "memory");
    return m & 8;
}

static void irq_restore(unsigned long m)
{
    __asm__ volatile ("csrs mstatus, %0" : : "r"(m) : "memory");
}

#define FETCH(N, T, NAME, EXPR)                                             \
    WEAK T __atomic_fetch_##NAME##_##N(volatile void *p, T v, int order)    \
    {                                                                       \
        volatile T *q = (volatile T *)p;                                    \
        unsigned long m = irq_off();                                        \
        T old = *q;                                                         \
        (void)order;                                                        \
        *q = (T)(EXPR);                                                     \
        irq_restore(m);                                                     \
        return old;                                                         \
    }                                                                       \
    WEAK T __atomic_##NAME##_fetch_##N(volatile void *p, T v, int order)    \
    {                                                                       \
        volatile T *q = (volatile T *)p;                                    \
        unsigned long m = irq_off();                                        \
        T old = *q, nv = (T)(EXPR);                                         \
        (void)order;                                                        \
        *q = nv;                                                            \
        irq_restore(m);                                                     \
        return nv;                                                          \
    }

#define SIZED(N, T)                                                         \
    FETCH(N, T, add, old + v)                                               \
    FETCH(N, T, sub, old - v)                                               \
    FETCH(N, T, and, old & v)                                               \
    FETCH(N, T, or, old | v)                                                \
    FETCH(N, T, xor, old ^ v)                                               \
    FETCH(N, T, nand, ~(old & v))                                           \
    WEAK T __atomic_load_##N(const volatile void *p, int order)             \
    {                                                                       \
        (void)order;                                                        \
        return *(const volatile T *)p;                                      \
    }                                                                       \
    WEAK void __atomic_store_##N(volatile void *p, T v, int order)          \
    {                                                                       \
        (void)order;                                                        \
        *(volatile T *)p = v;                                               \
    }                                                                       \
    WEAK T __atomic_exchange_##N(volatile void *p, T v, int order)          \
    {                                                                       \
        volatile T *q = (volatile T *)p;                                    \
        unsigned long m = irq_off();                                        \
        T old = *q;                                                         \
        (void)order;                                                        \
        *q = v;                                                             \
        irq_restore(m);                                                     \
        return old;                                                         \
    }                                                                       \
    WEAK _Bool __atomic_compare_exchange_##N(volatile void *p, void *exp,   \
                                             T des, int ok, int fail)       \
    {                                                                       \
        volatile T *q = (volatile T *)p;                                    \
        T *e = (T *)exp;                                                    \
        unsigned long m = irq_off();                                        \
        T cur = *q;                                                         \
        _Bool hit = cur == *e;                                              \
        (void)ok;                                                           \
        (void)fail;                                                         \
        if (hit)                                                            \
            *q = des;                                                       \
        else                                                                \
            *e = cur;                                                       \
        irq_restore(m);                                                     \
        return hit;                                                         \
    }

SIZED(1, unsigned char)
SIZED(2, unsigned short)
SIZED(4, unsigned int)
#if __riscv_xlen == 64
SIZED(8, unsigned long)
#endif

#endif
