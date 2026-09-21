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

extern "C" {

/* The ABI's guard is 8 bytes. Byte 0 is "initialised" -- the compiler
 * tests it inline before calling here, so the fast path is one load.
 * Byte 1 is this implementation's "in progress" flag, which is what makes
 * recursion detectable rather than a silent double-initialisation.
 *
 * Single-threaded: there is no thread library under this yet, so acquire
 * does not block. When threads arrive the fix is here and nowhere else --
 * which is the reason the ABI routes every initialisation through these
 * three functions instead of inlining a test-and-set. */
int __cxa_guard_acquire(std::uint64_t *g)
{
    unsigned char *b = reinterpret_cast<unsigned char *>(g);
    if (b[0])
        return 0;                    /* already done; do not initialise */
    if (b[1]) {
        /* The initialiser re-entered its own object. [stmt.dcl]/4 calls
         * this undefined; a deadlock is the usual outcome elsewhere and is
         * unhelpful. Refusing to recurse turns it into a crash at the
         * point of the bug. */
        __builtin_trap();
    }
    b[1] = 1;
    return 1;                        /* the caller runs the initialiser */
}

void __cxa_guard_release(std::uint64_t *g)
{
    unsigned char *b = reinterpret_cast<unsigned char *>(g);
    b[1] = 0;
    b[0] = 1;
}

/* The initialiser threw. The object is not constructed, so the guard goes
 * back to its starting state and the NEXT call tries again -- which is
 * what [stmt.dcl]/4 requires and the easiest thing to get wrong. */
void __cxa_guard_abort(std::uint64_t *g)
{
    unsigned char *b = reinterpret_cast<unsigned char *>(g);
    b[1] = 0;
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
