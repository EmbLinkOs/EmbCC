/* lib/rt's atomics on a Cortex-M in unprivileged Thread mode: PRIMASK
 * cannot be set there, so they take the RTOS's lock (__embcc_atomic_lock
 * and _unlock) instead of running unprotected; privileged, they mask
 * interrupts and never call it. With HOOKS undefined, the default lock
 * traps: the program faults (exit 125) rather than lose an update. */
_Atomic int a4;                /* __atomic_fetch_add_4: armv6m.c */
_Atomic long long a8;          /* __atomic_fetch_add_8: atomic8.c */

#ifdef HOOKS
static volatile int locks, unlocks;
void __embcc_atomic_lock(void) { locks++; }
void __embcc_atomic_unlock(void) { unlocks++; }
#endif

static void drop_privilege(void)
{
    unsigned c;
    __asm__ volatile ("mrs %0, control" : "=r"(c));
    c |= 1;                                      /* nPRIV */
    __asm__ volatile ("msr control, %0\n\tisb" : : "r"(c) : "memory");
}

int main(void)
{
    a4 += 5;
    a8 += 7;
#ifdef HOOKS
    if (locks || unlocks) return 1;              /* privileged: no hook */
#endif
    drop_privilege();
    a4 += 10;
    a8 += 20;
#ifdef HOOKS
    /* one each for every update lib/rt makes: both widths on ARMv6-M, the
     * 8-byte one on ARMv7-M (whose 4-byte one is ldrex/strex) */
    if (locks != NHOOK || unlocks != NHOOK) return 2;
#endif
    if (a4 != 15 || a8 != 27) return 3;
    return 42;
}
