/* The BLOCKING half of atomic wait/notify, driven through the seam.
 *
 * `atomics.cc` covers everything that returns without sleeping. What it
 * cannot reach is the part that is actually difficult: the order in
 * which a waiter reads the notification counter and checks the value,
 * and the order in which a notifier bumps that counter and wakes. Get
 * either backwards and a notification arriving in the window between a
 * waiter's check and its sleep is LOST -- the waiter blocks forever on
 * something that already happened. It is a race, so it does not show up
 * in a test that merely runs; it shows up once a year on a machine you
 * do not own.
 *
 * So this does not race. `lib/libc/os/posixlike/backend.c` declares
 * futex_wait and futex_wake WEAK precisely so a program can supply
 * them, and the two here are instrumented: they record what the library
 * asked for, and futex_wait plays the part of the other thread by
 * running the notifier inside the sleep. One thread, no timing.
 *
 * That buys the NOTIFIER's ordering, which check 4 pins down exactly --
 * swap the two lines in __notify and it fails. It does not buy the
 * waiter's: see the note after check 4 for why, and for what is done
 * instead. Claiming both would be the easy thing to write here and
 * would be false.
 */
#include <atomic>
#include <cstdio>
#include "check.h"

using namespace std;

static int wait_calls, wake_calls;
static int wait_expected;     /* the counter value the waiter slept on */
static int wake_word;         /* the counter as it stood inside the wake */
static const volatile int *wake_addr;

/* Set by a test to have the "other thread" run inside the sleep. */
static void (*during_sleep)();

extern "C" int futex_wait(const volatile int *addr, int expected, long)
{
    wait_calls++;
    wait_expected = expected;
    if (during_sleep) {
        void (*f)() = during_sleep;
        during_sleep = nullptr;   /* once: the second pass must not sleep */
        f();
    }
    return 0;                     /* as if woken; the caller re-checks */
}

extern "C" int futex_wake(const volatile int *addr, int count)
{
    wake_calls++;
    wake_addr = addr;
    wake_word = __atomic_load_n((const int *)addr, __ATOMIC_ACQUIRE);
    (void)count;
    return 0;
}

static void reset() { wait_calls = wake_calls = 0; during_sleep = nullptr; }

static atomic<int> g{0};
static void flip_and_notify() { g.store(1); g.notify_all(); }

int main()
{
    /* The whole file is meaningless if the seam did not pick up the weak
     * definitions above -- every check would pass against a futex that
     * was never called. So prove they are reached first. */
    {
        reset();
        atomic<int> a{5};
        a.notify_one();
        CHECK(wake_calls == 1);
    }

    /* 1. A value that already differs must not sleep AT ALL. Sleeping
     *    and being woken would also "work", and would be a bug: there is
     *    no guarantee anyone will ever wake it. */
    {
        reset();
        atomic<int> a{7};
        a.wait(6);
        CHECK(wait_calls == 0);
    }

    /* 2. A value that matches sleeps, and stops once it changes. */
    {
        reset();
        g.store(0);
        during_sleep = flip_and_notify;
        g.wait(0);
        CHECK(wait_calls == 1);       /* slept once */
        CHECK(g.load() == 1);
    }

    /* 3. Each notify moves the counter. Necessary but not sufficient --
     *    it would hold with the bump on either side of the wake, which
     *    is what 4 is for. */
    {
        reset();
        atomic<int> a{0};
        a.notify_all();
        int first = wake_word;
        a.notify_all();
        CHECK(wake_calls == 2);
        CHECK(wake_word == first + 1);
    }

    /* 4. The notifier bumps the counter BEFORE it wakes.
     *
     *    Observed across the two operations rather than inside one. A
     *    notify records the counter as the WAKE saw it; a wait right
     *    after records the counter the SLEEP was conditioned on. With
     *    the bump first those are the same number. With the wake first
     *    the waiter sleeps on a counter one greater than the notifier
     *    ever published -- which is exactly how a notification goes
     *    missing, and the check fails if the two lines are swapped. */
    {
        reset();
        g.store(0);
        g.notify_all();               /* moves the counter to a known value */
        int after_notify = wake_word;
        during_sleep = flip_and_notify;
        g.wait(0);
        CHECK(wait_calls == 1);
        CHECK(wait_expected == after_notify);
    }

    /* Not checked here, and worth saying so: that the WAITER reads the
     * counter before it checks the value. Telling the two orders apart
     * needs a notification delivered between those two steps, and there
     * is no seam between them to hang one on -- both are inline loads.
     * A second thread could hit the window by racing, which is the kind
     * of test that passes for a year and then does not. The order is
     * argued in <atomic> instead, and the asymmetry is real: this file
     * pins the notifier's half and reasons about the waiter's. */

    /* 5. And the address is the one the notifier woke: waiter and
     *    notifier must agree on the bucket, or they never meet. */
    {
        reset();
        g.store(0);
        during_sleep = flip_and_notify;
        g.wait(0);
        const volatile int *woken = wake_addr;
        reset();
        g.store(2);
        g.notify_one();
        CHECK(wake_addr == woken);
    }

    /* 6. A notify does not touch the value -- it moves a counter that
     *    lives beside it, which is why an object of any size can be
     *    waited on with a 32-bit primitive. */
    {
        reset();
        atomic<long long> big{0x1122334455667788LL};
        big.notify_all();
        CHECK(big.load() == 0x1122334455667788LL);
        CHECK(wake_calls == 1);

        atomic_flag f = ATOMIC_FLAG_INIT;
        f.notify_all();
        CHECK(!f.test());
    }

    DONE();
}
