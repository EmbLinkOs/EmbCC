/* CMSIS's shapes on a Cortex-M0: an intrinsic is a static inline function
 * around one asm, and a critical section is two of them (ARMv6-M has
 * PRIMASK, not BASEPRI). */
#define INLINE static inline __attribute__((always_inline))

INLINE unsigned get_primask(void)
{
    unsigned r;
    __asm volatile("mrs %0, primask" : "=r"(r) :: "memory");
    return r;
}

INLINE void set_primask(unsigned v)
{
    __asm volatile("msr primask, %0" :: "r"(v) : "memory");
}

int bump_primask(volatile int *p)
{
    unsigned s = get_primask();
    __asm volatile("cpsid i" ::: "memory");
    int v = *p + 2;
    *p = v;
    set_primask(s);
    return v;
}

unsigned primask(void)
{
    return get_primask();
}
