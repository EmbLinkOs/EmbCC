/* The AVR library's exit-time registrations (lib/avr): __cxa_atexit,
 * which C++ registers each static object's destructor with, and atexit.
 *
 * avr-libc has neither -- its exit runs .fini sections and stops -- and
 * on a part whose main never returns they cost nothing, so they are only
 * linked when something registers: then exit here, strong, replaces the
 * startup's weak `exit = _exit` and runs what was registered, last
 * first, before _exit. The table is small and fixed, as RAM is: eight
 * entries, a registration past them failing as the standard allows. */
#include <stdint.h>

#ifndef EMB_AVR_ATEXIT_MAX
#define EMB_AVR_ATEXIT_MAX 8
#endif

#ifdef EMB_AVR_CXA_ATEXIT
extern void _exit(int status) __attribute__((noreturn));

static struct { void (*fn)(void *); void *arg; } tab[EMB_AVR_ATEXIT_MAX];
static uint8_t ntab;

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (ntab == EMB_AVR_ATEXIT_MAX)
        return -1;
    tab[ntab].fn = fn;
    tab[ntab].arg = arg;
    ntab++;
    return 0;
}

int atexit(void (*fn)(void))
{
    return __cxa_atexit((void (*)(void *))fn, 0, 0);
}

void exit(int status)
{
    while (ntab) {
        ntab--;
        tab[ntab].fn(tab[ntab].arg);
    }
    _exit(status);
}
#endif

#ifdef EMB_AVR_OS_THREAD_YIELD
/* what a C++ guard's waiter calls while another thread initializes the
 * object: there is no other thread */
void __os_thread_yield(void)
{
}
#endif

#ifdef EMB_AVR_OS_THREAD_SELF
/* ...and who is initializing it: the one thread there is */
unsigned long __os_thread_self(void)
{
    return 1;
}
#endif

#ifdef EMB_AVR_OS_FUTEX_WAIT
/* a wait on a word: with one thread nothing could change it, so the
 * waiter is told to go and look again (-1), as a spurious wake is */
int __os_futex_wait(const volatile int *addr, int expected, long timeout_ns)
{
    (void)addr;
    (void)expected;
    (void)timeout_ns;
    return -1;
}
#endif

#ifdef EMB_AVR_OS_FUTEX_WAKE
int __os_futex_wake(const volatile int *addr, int count)
{
    (void)addr;
    (void)count;
    return 0;
}
#endif
