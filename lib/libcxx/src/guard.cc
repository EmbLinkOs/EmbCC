/* Static local initialisation, atexit, and the pure-virtual trap.
 * Itanium C++ ABI §3.3.2, §3.3.3, §2.9.
 *
 * `static T x = f();` inside a function has to run f() exactly once, on
 * the first call, and every later call must see the finished object. The
 * compiler emits a guard variable and these three calls around the
 * initialiser; what they mean is a one-shot latch.
 */
#include <cstddef>
#include <cstdint>

#include "../../libc/os/backend.h"

extern "C" {

/* The ABI's guard is 8 bytes. Byte 0 is "initialised" -- the compiler
 * tests it inline before calling here, so the fast path is one load.
 * Byte 1 is this implementation's "in progress" flag, which is what makes
 * recursion detectable rather than a silent double-initialisation, and
 * which now also carries the OWNER: a thread that finds byte 1 set has
 * to know whether the initialiser is its own (recursion, a bug) or
 * another thread's (a wait, and correct). Bytes 4..7 hold a word the
 * waiters sleep on.
 *
 * This used to say "single-threaded: when threads arrive the fix is here
 * and nowhere else", which is the reason the ABI routes every
 * initialisation through three functions instead of inlining a
 * test-and-set. Threads arrived. This is that fix.
 *
 * [stmt.dcl]/4 is explicit about what is required: if control enters the
 * declaration concurrently while the variable is being initialised, the
 * concurrent execution WAITS for the initialisation to complete. Not
 * "may" -- so two threads reaching a function-local static must produce
 * exactly one initialisation, and the loser must not proceed until the
 * object exists. */
namespace {

/* The guard, as this implementation lays it out. The layout is private
 * to these three functions; the compiler only ever tests byte 0.
 *
 * On 32-bit ARM the guard is one 32-bit word (the ARM C++ ABI, 3.2.3),
 * and the compiler tests its bit 0 -- byte 0 on a little-endian core, so
 * `done` stays where it is. There is no room for the wait word, so the
 * waiters of every guard sleep on one shared word: a release wakes them
 * all, and each re-tests its own guard, which they do anyway. */
#ifdef __ARM_EABI__
typedef unsigned guard_word;
volatile int shared_wait;

struct guard_t {
    unsigned char done;      /* byte 0 (bit 0): initialised */
    unsigned char busy;      /* byte 1: an initialisation is running */
    unsigned char pad[2];
};

volatile int *wait_word(guard_t *) { return &shared_wait; }
#else
typedef std::uint64_t guard_word;

struct guard_t {
    unsigned char done;      /* byte 0: initialised, the inline fast path */
    unsigned char busy;      /* byte 1: an initialisation is running */
    unsigned char pad[2];
    volatile int  wait;      /* bytes 4..7: bumped on release, slept on */
};

volatile int *wait_word(guard_t *g) { return &g->wait; }
#endif

/* Who is running which initialiser, so that a thread finding a guard
 * busy can tell RECURSION (its own, a bug) from CONTENTION (another
 * thread's, a wait). A guard is eight bytes and every one of them is
 * spoken for, so the pairing lives outside it.
 *
 * In a global table and NOT in thread_local storage, which is what this
 * first used. Most targets this library runs on have no TLS runtime at
 * all: the bare-metal harnesses never set up a thread pointer, so a
 * single `__thread int` there is a fault, and a guard that touched one
 * crashed every C++ program on those targets. The C++ library cannot
 * depend on a facility that only the hosted targets have.
 *
 * The table is small because what it holds is small: one entry per
 * initialiser RUNNING right now, which is (nesting depth x threads).
 * If it ever fills, recursion simply stops being detected for the
 * overflowing entry -- the initialisation still happens exactly once,
 * and the undefined case goes back to being a deadlock instead of a
 * trap. Losing a diagnostic is the right failure; refusing to
 * initialise would not be. */
struct owner { guard_t *g; unsigned long thr; };

owner owners[64];
volatile int nowners;
volatile int owners_lock;

/* A plain test-and-set, not the futex mutex: the critical section is a
 * scan of at most 64 entries with no syscall in it, so a waiter is
 * never asleep for long, and this has to work on targets where there is
 * no futex to sleep on. */
void owners_acquire()
{
    while (__atomic_exchange_n(&owners_lock, 1, __ATOMIC_ACQUIRE))
        __os_thread_yield();
}

void owners_release()
{
    __atomic_store_n(&owners_lock, 0, __ATOMIC_RELEASE);
}

void own(guard_t *g)
{
    owners_acquire();
    if (nowners < (int)(sizeof owners / sizeof *owners)) {
        owners[nowners].g = g;
        owners[nowners].thr = __os_thread_self();
        nowners++;
    }
    owners_release();
}

void disown(guard_t *g)
{
    owners_acquire();
    for (int i = nowners - 1; i >= 0; i--)
        if (owners[i].g == g) {
            owners[i] = owners[--nowners];
            break;
        }
    owners_release();
}

bool owned_by_me(guard_t *g)
{
    unsigned long me = __os_thread_self();
    bool mine = false;
    owners_acquire();
    for (int i = 0; i < nowners; i++)
        if (owners[i].g == g && owners[i].thr == me) {
            mine = true;
            break;
        }
    owners_release();
    return mine;
}

}  // namespace

int __cxa_guard_acquire(guard_word *g)
{
    guard_t *gd = reinterpret_cast<guard_t *>(g);
    for (;;) {
        if (__atomic_load_n(&gd->done, __ATOMIC_ACQUIRE))
            return 0;                /* already done; do not initialise */

        /* `busy` from 0 to 1, as a compare-and-swap of the guard's first
         * word -- done, busy and the padding -- from all zero: RV32 has
         * no byte-wide one, and the word's other bytes are zero exactly
         * when the byte's would have succeeded (done is set only by the
         * thread that set busy). */
        guard_t claimed = {};
        claimed.busy = 1;
        unsigned expect = 0, busy_word;
        __builtin_memcpy(&busy_word, &claimed, sizeof busy_word);
        if (__atomic_compare_exchange_n(reinterpret_cast<unsigned *>(gd),
                                        &expect, busy_word, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            own(gd);
            return 1;                /* the caller runs the initialiser */
        }

        if (owned_by_me(gd)) {
            /* The initialiser re-entered its own object. [stmt.dcl]/4
             * calls this undefined; a deadlock is the usual outcome
             * elsewhere and is unhelpful, and it would now be
             * indistinguishable from the legitimate wait below.
             * Refusing to recurse turns it into a crash at the point of
             * the bug. */
            __builtin_trap();
        }

        /* Another thread is initialising it. Wait for the release, on
         * the word inside the guard -- reading it BEFORE re-testing
         * `busy`, so a release between the two is not missed: the wait
         * compares the word and returns at once if it has changed. */
        volatile int *w = wait_word(gd);
        int seen = __atomic_load_n(w, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&gd->busy, __ATOMIC_ACQUIRE) &&
            !__atomic_load_n(&gd->done, __ATOMIC_ACQUIRE)) {
            if (__os_futex_wait(w, seen, -1) < 0)
                __os_thread_yield();
        }
    }
}

static void guard_wake(guard_t *gd)
{
    volatile int *w = wait_word(gd);
    __atomic_fetch_add(w, 1, __ATOMIC_RELEASE);
    __os_futex_wake(w, -1);              /* every waiter: all may proceed */
}

void __cxa_guard_release(guard_word *g)
{
    guard_t *gd = reinterpret_cast<guard_t *>(g);
    disown(gd);
    /* `done` is published BEFORE `busy` is cleared, so a thread that
     * sees the guard idle can only see it finished -- never idle and
     * uninitialised, which would run the initialiser twice. */
    __atomic_store_n(&gd->done, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&gd->busy, 0, __ATOMIC_RELEASE);
    guard_wake(gd);
}

/* The initialiser threw. The object is not constructed, so the guard goes
 * back to its starting state and the NEXT call tries again -- which is
 * what [stmt.dcl]/4 requires and the easiest thing to get wrong. */
void __cxa_guard_abort(guard_word *g)
{
    guard_t *gd = reinterpret_cast<guard_t *>(g);
    disown(gd);
    __atomic_store_n(&gd->busy, 0, __ATOMIC_RELEASE);
    /* The waiters have to be woken here too, and they must re-test
     * rather than assume: `done` is still 0, so one of them takes the
     * guard and tries the initialiser again, which is what
     * [stmt.dcl]/4 asks for. */
    guard_wake(gd);
}

/* __cxa_atexit and __cxa_finalize are NOT here. They live in the C
 * library, next to exit(), because C++ static destructors and C atexit
 * handlers must interleave by registration order -- and two lists cannot
 * express that ordering however they are drained. See
 * lib/libc/src/stdlib/exit.c.
 */

/* The compiler puts this in a vtable slot for a pure virtual function. It
 * is reached only by calling one during construction or destruction of the
 * abstract base, which is undefined behaviour with a specific cause worth
 * stopping on rather than a random jump. */
void __cxa_pure_virtual(void)  { __builtin_trap(); }
void __cxa_deleted_virtual(void) { __builtin_trap(); }

}  // extern "C"
