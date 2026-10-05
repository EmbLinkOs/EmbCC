/* A RISC-V RTOS's shape: a critical section is a CSR read and two
 * writes, each a static inline function around one asm. */
#define INLINE static inline __attribute__((always_inline))

INLINE unsigned long read_mstatus(void)
{
    unsigned long r;
    __asm volatile("csrr %0, mstatus" : "=r"(r) :: "memory");
    return r;
}

INLINE void write_mstatus(unsigned long v)
{
    __asm volatile("csrw mstatus, %0" :: "r"(v) : "memory");
}

int bump(volatile int *p)
{
    unsigned long s = read_mstatus();
    write_mstatus(s & ~8UL);
    int v = *p + 1;
    *p = v;
    write_mstatus(s);
    return v;
}

unsigned long status(void)
{
    return read_mstatus();
}
