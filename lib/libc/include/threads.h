/* <threads.h> — C11 §7.26.
 *
 * C's threads, over the same OS seam the C++ library uses
 * (lib/libc/os/backend.h). One set of primitives, two spellings: a
 * target that gains threads lights up both at once, and neither library
 * knows which OS it is on.
 *
 * ---- what a target without threads reports ------------------------------
 *
 * `thrd_error`, from `thrd_create`. Not `thrd_success` with a thread
 * that never runs, and not a thread that runs on the calling thread --
 * the second deadlocks the first time anything joins from inside it,
 * and the first is a lie a caller cannot detect. Everything that does
 * not need a second thread works: mutexes, once_flag, and the sleeps.
 *
 * ---- the one interface difference from C++ -------------------------------
 *
 * A C thread's function returns an `int` and C++'s returns nothing. The
 * value goes to `thrd_join`'s out parameter, which is why joining takes
 * one. The trampoline below holds it, because the seam's primitive
 * takes a `void (*)(void *)`: putting the return value in the seam
 * would make every backend carry a C-specific detail.
 */
#ifndef _THREADS_H
#define _THREADS_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C11 says `thread_local` is a macro for `_Thread_local`. EmbCC does
 * not implement thread-local storage, so a target with threads would
 * need it before this means anything -- and saying so is better than
 * defining a macro that compiles to a plain global shared by every
 * thread, which is the failure mode nobody notices until two threads
 * corrupt each other. */
#if !defined(__cplusplus) && defined(__EMBCC_HAS_THREAD_LOCAL__)
#define thread_local _Thread_local
#endif

enum {
    thrd_success  = 0,
    thrd_busy     = 1,
    thrd_error    = 2,
    thrd_nomem    = 3,
    thrd_timedout = 4
};

enum {
    mtx_plain     = 0,
    mtx_recursive = 1,
    mtx_timed     = 2
};

typedef unsigned long thrd_t;
typedef int (*thrd_start_t)(void *);

/* The state is the same word a futex sleeps on, so these are the same
 * locks the C++ <mutex> builds -- one implementation, not two. */
typedef struct {
    int __state;
    int __type;
    unsigned long __owner;
    unsigned __count;
} mtx_t;

typedef struct {
    int __seq;
} cnd_t;

typedef struct {
    int __state;
} once_flag;

#define ONCE_FLAG_INIT { 0 }

/* Thread-specific storage needs thread-local storage underneath, which
 * this compiler does not have; tss_create reports failure rather than
 * handing back a key that every thread shares. */
typedef int tss_t;
typedef void (*tss_dtor_t)(void *);
#define TSS_DTOR_ITERATIONS 4

int  thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int  thrd_equal(thrd_t a, thrd_t b);
thrd_t thrd_current(void);
int  thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void thrd_yield(void);
/* `_Noreturn` is C's spelling and `[[noreturn]]` is C++'s, and this
 * header is included from both -- <assert.h> makes the same split for
 * the same reason. */
#ifdef __cplusplus
[[noreturn]] void thrd_exit(int res);
#else
_Noreturn void thrd_exit(int res);
#endif
int  thrd_detach(thrd_t thr);
int  thrd_join(thrd_t thr, int *res);

int  mtx_init(mtx_t *mtx, int type);
int  mtx_lock(mtx_t *mtx);
int  mtx_timedlock(mtx_t *__restrict mtx, const struct timespec *__restrict ts);
int  mtx_trylock(mtx_t *mtx);
int  mtx_unlock(mtx_t *mtx);
void mtx_destroy(mtx_t *mtx);

int  cnd_init(cnd_t *cond);
int  cnd_signal(cnd_t *cond);
int  cnd_broadcast(cnd_t *cond);
int  cnd_wait(cnd_t *cond, mtx_t *mtx);
int  cnd_timedwait(cnd_t *__restrict cond, mtx_t *__restrict mtx,
                   const struct timespec *__restrict ts);
void cnd_destroy(cnd_t *cond);

void call_once(once_flag *flag, void (*func)(void));

int  tss_create(tss_t *key, tss_dtor_t dtor);
void *tss_get(tss_t key);
int  tss_set(tss_t key, void *val);
void tss_delete(tss_t key);

#ifdef __cplusplus
}
#endif

#endif
