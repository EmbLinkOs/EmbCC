/* What <pthread.h> and <threads.h> share: one table of the threads this
 * library started, thread-specific data, and the futex word every lock
 * here is made of. C11's thrd_* are pthread_* with other spellings, so
 * there is one implementation of each idea, in pthread.c. */
#ifndef EMBLIBC_THR_H
#define EMBLIBC_THR_H

#include <setjmp.h>
#include <stddef.h>
#include <time.h>

#define __THR_KEYS 128                /* PTHREAD_KEYS_MAX */
#define __THR_DTOR_ROUNDS 4           /* PTHREAD_DESTRUCTOR_ITERATIONS */

struct __thr {
    struct __thr *next;
    unsigned long id;
    void *(*fn)(void *);
    void *arg;
    void *result;
    int detached;                     /* 1 detached, 2 being joined */
    int done, ours;                   /* ours: started here, so exit can unwind */
    int registered;                   /* pthread_create has returned */
    int cancel_state, cancel_type;
    jmp_buf exit_to;
    void *tsd[__THR_KEYS];
    char name[16];
    int tid;                          /* the kernel's id for it (gettid) */
    void *aff;                        /* a CPU set to take on starting */
    size_t affsize;
};

#ifndef WEAK
#define WEAK __attribute__((weak))
#endif

/* pthread_create with the reason kept: ENOMEM apart from EAGAIN */
int __thr_create(unsigned long *out, const void *attr,
                 void *(*fn)(void *), void *arg);

/* fork's three moments (proc.c calls it through __atfork_hook, which
 * the thread library sets as soon as it is used) */
void __pthread_atfork_run(int phase);
extern void (*__atfork_hook)(int phase);

/* this thread's record, made on first need for one this library did not
 * start (the program's first thread, a C++ std::thread) */
struct __thr *__thr_self(void);

/* the futex word: 0 free, 1 held, 2 held and maybe waited on. abs is a
 * deadline on `clock`, NULL for none; 0, or ETIMEDOUT */
int  __word_lock(volatile int *w, const struct timespec *abs, clockid_t clock);
int  __word_trylock(volatile int *w);
void __word_unlock(volatile int *w);

/* nanoseconds from now until abs on clock, at least 0; -1 for no abs */
long __thr_until(const struct timespec *abs, clockid_t clock);

#endif
