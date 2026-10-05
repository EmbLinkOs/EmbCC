/* CMSIS's and FreeRTOS's shapes: an intrinsic is a static inline function
 * around one asm, and a critical section is two of them. */
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

/* FreeRTOS ARM_CM3 ulPortRaiseBASEPRI: two outputs, one a scratch */
INLINE unsigned raise_basepri(void)
{
    unsigned old, new_;
    __asm volatile("mrs %0, basepri\n"
                   "mov %1, %2\n"
                   "msr basepri, %1\n"
                   "isb\n"
                   "dsb\n"
                   : "=r"(old), "=r"(new_) : "i"(0xa0) : "memory");
    return old;
}

INLINE void set_basepri(unsigned v)
{
    __asm volatile("msr basepri, %0" :: "r"(v) : "memory");
}

int bump(volatile int *p)
{
    unsigned s = raise_basepri();
    int v = *p + 1;
    *p = v;
    set_basepri(s);
    return v;
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

unsigned raised(void)
{
    return raise_basepri();
}
