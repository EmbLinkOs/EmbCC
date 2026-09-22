/* The one mutex this library has.
 *
 * It exists because the library has global state -- the heap's block
 * lists, a FILE's buffer and position -- and threads arrived after that
 * state was written. Two threads in malloc at once corrupt the free
 * list, which shows up as a segfault somewhere else entirely; two
 * threads in printf at once interleave characters inside a line.
 *
 * It is built on the futex seam (`lib/libc/os/backend.h`) rather than on
 * an OS mutex, for the reason the seam gives: a mutex primitive pushes
 * the fairness and recursion policy into every backend, while a word in
 * memory that a thread can sleep on is enough to build one here, once.
 *
 * The shape is the standard three-state futex mutex:
 *
 *      0  free
 *      1  held, nobody waiting
 *      2  held, and somebody MIGHT be waiting
 *
 * The point of the third state is that an uncontended lock and unlock
 * are one atomic instruction each and no syscall at all -- which is what
 * makes it acceptable to put a lock around malloc on a target that has
 * one thread. The state can only be over-estimated (a 2 with nobody
 * actually waiting costs one wasted wake), never under-estimated, which
 * is what keeps it correct.
 *
 * On a target with no futex -- every freestanding one, where
 * `__os_futex_wait` returns ENOSYS -- the wait degrades to a yield. That
 * is a spin, and it is fine precisely there: a target with no futex has
 * no threads, so the lock is never contended and the spin never runs.
 */
#ifndef EMBLIBC_LOCK_H
#define EMBLIBC_LOCK_H

typedef struct { volatile int v; } __lock_t;

#define LOCK_INIT { 0 }

void __lock(__lock_t *l);
void __unlock(__lock_t *l);

#endif
