/* POSIX threads and C11's: creating, joining, exiting with a value from
 * deep inside, thread-specific data and its destructors, the three
 * kinds of mutex, condition variables with a deadline on either clock,
 * read-write locks, barriers, once, spin locks, semaphores, fork's
 * handlers, each thread's own errno and __thread variables -- and C11's
 * spellings, whose deadlines are absolute.
 *
 * Every answer is glibc's (the program ran against glibc 2.36 too).
 * Where the target has no threads, pthread_create fails with EAGAIN and
 * only what one thread can show is checked: keys, error-checking
 * mutexes, once, semaphores without waiting. */
// expect-exit: 42
// needs-libc: POSIX threads, from lib/libc
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static int threads_ok;
static int clock_ok;           /* a board may have no clock: no timing then */

static struct timespec in_ms(clockid_t c, long ms)
{
    struct timespec t = { 0, 0 };  /* where there is no clock: from 0 */
    clock_gettime(c, &t);
    t.tv_nsec += ms % 1000 * 1000000L;
    t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    return t;
}

static long elapsed_ms(const struct timespec *a)
{
    struct timespec b;
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
}

/* ---- create, join, exit, keys ---------------------------------------------- */

static pthread_key_t g_key;
static int g_dtor_runs, g_dtor_value;

static void dtor(void *v)
{
    g_dtor_runs++;
    g_dtor_value += (int)(intptr_t)v;
    /* set again: POSIX gives it another round */
    if ((intptr_t)v == 7)
        pthread_setspecific(g_key, (void *)(intptr_t)8);
}

static void deep_exit(int n)
{
    if (n == 0)
        pthread_exit((void *)(intptr_t)99);
    deep_exit(n - 1);
}

static void *worker(void *arg)
{
    pthread_setspecific(g_key, (void *)(intptr_t)7);
    if ((intptr_t)arg == 1)
        deep_exit(5);
    return (void *)(intptr_t)((intptr_t)arg * 10);
}

static int basics(void)
{
    if (pthread_key_create(&g_key, dtor))
        return 1;
    pthread_setspecific(g_key, (void *)(intptr_t)5);
    if (pthread_getspecific(g_key) != (void *)(intptr_t)5)
        return 2;
    if (!pthread_equal(pthread_self(), pthread_self()))
        return 3;
    if (pthread_join(pthread_self(), NULL) != EDEADLK)
        return 4;

    pthread_t t;
    int e = pthread_create(&t, NULL, worker, (void *)(intptr_t)4);
    if (e == EAGAIN)
        return 0;                  /* a target with no threads */
    if (e)
        return 5;
    threads_ok = 1;
    void *r;
    if (pthread_join(t, &r) || r != (void *)(intptr_t)40)
        return 6;
    /* the destructor ran twice in that thread (7, then the 8 it set),
     * and this thread's value is untouched */
    if (g_dtor_runs != 2 || g_dtor_value != 15 ||
        pthread_getspecific(g_key) != (void *)(intptr_t)5)
        return 7;
    if (pthread_create(&t, NULL, worker, (void *)(intptr_t)1) ||
        pthread_join(t, &r) || r != (void *)(intptr_t)99 || g_dtor_runs != 4)
        return 8;

    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, worker, (void *)(intptr_t)2))
        return 9;
    size_t ss;
    if (pthread_attr_setstacksize(&a, 1 << 20) ||
        pthread_attr_getstacksize(&a, &ss) || ss != 1 << 20 ||
        pthread_attr_setstacksize(&a, 1) != EINVAL ||
        pthread_attr_setscope(&a, PTHREAD_SCOPE_PROCESS) != ENOTSUP)
        return 10;
    pthread_attr_destroy(&a);
    usleep(50000);             /* let the detached one finish */
    return 0;
}

/* ---- mutexes --------------------------------------------------------------- */

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static long g_count;

static void *bump(void *arg)
{
    for (int i = 0; i < 5000; i++) {
        pthread_mutex_lock(&g_m);
        g_count++;
        pthread_mutex_unlock(&g_m);
    }
    return arg;
}

static void *try_held(void *arg)
{
    pthread_mutex_t *m = arg;
    struct timespec d = in_ms(CLOCK_REALTIME, 50);
    int busy = pthread_mutex_trylock(m);
    int timed = pthread_mutex_timedlock(m, &d);
    return (void *)(intptr_t)(busy == EBUSY && timed == ETIMEDOUT);
}

static int mutexes(void)
{
    pthread_mutexattr_t at;
    pthread_mutex_t m;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m, &at);
    if (pthread_mutex_lock(&m) || pthread_mutex_lock(&m) ||
        pthread_mutex_unlock(&m) || pthread_mutex_unlock(&m) ||
        pthread_mutex_unlock(&m) != EPERM)
        return 20;
    pthread_mutex_destroy(&m);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&m, &at);
    if (pthread_mutex_lock(&m) || pthread_mutex_lock(&m) != EDEADLK ||
        pthread_mutex_destroy(&m) != EBUSY || pthread_mutex_unlock(&m) ||
        pthread_mutex_unlock(&m) != EPERM)
        return 21;
    if (pthread_mutexattr_setpshared(&at, 2) != EINVAL)
        return 22;
    if (!threads_ok)
        return 0;

    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, bump, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    if (g_count != 20000)
        return 23;

    pthread_mutex_init(&m, NULL);
    pthread_mutex_lock(&m);
    void *r;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_create(&t[0], NULL, try_held, &m);
    pthread_join(t[0], &r);
    long ms = elapsed_ms(&t0);
    pthread_mutex_unlock(&m);
    if (!r || ms < 40 || ms > 2000)
        return 24;
    return 0;
}

/* ---- condition variables ------------------------------------------------- */

static pthread_cond_t g_c = PTHREAD_COND_INITIALIZER;
static int g_ready;

static void *producer(void *arg)
{
    usleep(20000);
    pthread_mutex_lock(&g_m);
    g_ready = 1;
    pthread_cond_signal(&g_c);
    pthread_mutex_unlock(&g_m);
    return arg;
}

static int conds(void)
{
    /* a deadline that passes: ETIMEDOUT, on either clock */
    pthread_mutex_lock(&g_m);
    struct timespec d = in_ms(CLOCK_REALTIME, 30), t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (pthread_cond_timedwait(&g_c, &g_m, &d) != ETIMEDOUT ||
        (clock_ok && elapsed_ms(&t0) < 20))
        return 30;
    pthread_condattr_t ca;
    pthread_cond_t mc;
    pthread_condattr_init(&ca);
    if (pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) ||
        pthread_condattr_setclock(&ca, CLOCK_PROCESS_CPUTIME_ID) != EINVAL)
        return 31;
    pthread_cond_init(&mc, &ca);
    d = in_ms(CLOCK_MONOTONIC, 30);
    if (pthread_cond_timedwait(&mc, &g_m, &d) != ETIMEDOUT)
        return 32;
    d.tv_nsec = 1000000000L;
    if (pthread_cond_timedwait(&mc, &g_m, &d) != EINVAL)
        return 33;
    pthread_mutex_unlock(&g_m);
    if (!threads_ok)
        return 0;

    pthread_t t;
    pthread_mutex_lock(&g_m);
    pthread_create(&t, NULL, producer, NULL);
    while (!g_ready)
        pthread_cond_wait(&g_c, &g_m);
    pthread_mutex_unlock(&g_m);
    pthread_join(t, NULL);
    return 0;
}

/* ---- read-write locks, barriers, once, spin ------------------------------- */

static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_barrier_t g_bar;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_once_runs, g_serial, g_in_read, g_max_read;
static pthread_spinlock_t g_spin;
static long g_spun;

static void once_fn(void) { g_once_runs++; }

/* errno and this are each thread's own, and this one starts as the
 * program wrote it: a new thread's block is a copy of the template, not
 * of whatever thread made it */
static __thread int t_mine = 5;
static int g_own_bad;

static void *crowd(void *arg)
{
    int me = (int)(intptr_t)arg;
    if (t_mine != 5)
        __atomic_add_fetch(&g_own_bad, 1, __ATOMIC_SEQ_CST);
    t_mine = 10 + me;
    errno = 100 + me;
    pthread_once(&g_once, once_fn);
    pthread_rwlock_rdlock(&g_rw);
    int n = __atomic_add_fetch(&g_in_read, 1, __ATOMIC_SEQ_CST);
    if (n > g_max_read)
        g_max_read = n;
    if (pthread_barrier_wait(&g_bar) == PTHREAD_BARRIER_SERIAL_THREAD)
        __atomic_add_fetch(&g_serial, 1, __ATOMIC_SEQ_CST);
    /* every thread has written both by now: a shared one would hold the
     * last writer's value in three of the four */
    if (errno != 100 + me || t_mine != 10 + me)
        __atomic_add_fetch(&g_own_bad, 1, __ATOMIC_SEQ_CST);
    __atomic_sub_fetch(&g_in_read, 1, __ATOMIC_SEQ_CST);
    pthread_rwlock_unlock(&g_rw);
    for (int i = 0; i < 2000; i++) {
        pthread_spin_lock(&g_spin);
        g_spun++;
        pthread_spin_unlock(&g_spin);
    }
    return arg;
}

static int others(void)
{
    if (pthread_rwlock_wrlock(&g_rw) || pthread_rwlock_wrlock(&g_rw) != EDEADLK ||
        pthread_rwlock_tryrdlock(&g_rw) != EBUSY ||
        pthread_rwlock_unlock(&g_rw) ||
        pthread_rwlock_rdlock(&g_rw) || pthread_rwlock_rdlock(&g_rw) ||
        pthread_rwlock_trywrlock(&g_rw) != EBUSY ||
        pthread_rwlock_unlock(&g_rw) || pthread_rwlock_unlock(&g_rw))
        return 40;
    if (pthread_barrier_init(&g_bar, NULL, 0) != EINVAL)
        return 41;
    pthread_spin_init(&g_spin, PTHREAD_PROCESS_PRIVATE);
    if (pthread_spin_trylock(&g_spin) || pthread_spin_trylock(&g_spin) != EBUSY ||
        pthread_spin_unlock(&g_spin))
        return 46;
    if (!threads_ok) {
        pthread_once(&g_once, once_fn);
        pthread_once(&g_once, once_fn);
        return g_once_runs == 1 ? 0 : 43;
    }
    /* four readers inside at once, which the barrier proves: none can
     * pass it until all four have the read lock */
    pthread_barrier_init(&g_bar, NULL, 4);
    pthread_t t[4];
    t_mine = 1;
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, crowd, (void *)(intptr_t)i);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    pthread_barrier_destroy(&g_bar);
    if (g_once_runs != 1 || g_serial != 1 || g_max_read != 4 || g_spun != 8000)
        return 44;
    if (g_own_bad || t_mine != 1)
        return 45;
    return 0;
}

/* ---- semaphores ------------------------------------------------------------- */

static sem_t g_sem;

static void *poster(void *arg)
{
    usleep(20000);
    sem_post(&g_sem);
    return arg;
}

static int sems(void)
{
    int v;
    if (sem_init(&g_sem, 0, 2) || sem_trywait(&g_sem) || sem_trywait(&g_sem))
        return 50;
    errno = 0;
    if (sem_trywait(&g_sem) != -1 || errno != EAGAIN)
        return 51;
    struct timespec d = in_ms(CLOCK_REALTIME, 30);
    errno = 0;
    if (sem_timedwait(&g_sem, &d) != -1 || errno != ETIMEDOUT)
        return 52;
    if (sem_post(&g_sem) || sem_getvalue(&g_sem, &v) || v != 1 ||
        sem_wait(&g_sem))
        return 53;
    if (threads_ok) {
        pthread_t t;
        pthread_create(&t, NULL, poster, NULL);
        if (sem_wait(&g_sem))
            return 54;
        pthread_join(t, NULL);
    }
    sem_destroy(&g_sem);
    return 0;
}

/* ---- C11's spellings ------------------------------------------------------- */

static tss_t g_tss;
static int g_tss_dtor;

static void tss_dtor(void *v) { g_tss_dtor += (int)(intptr_t)v; }

static int c11_worker(void *arg)
{
    tss_set(g_tss, (void *)(intptr_t)3);
    if ((intptr_t)arg == 1)
        thrd_exit(17);
    return 5;
}

static int c11(void)
{
    if (tss_create(&g_tss, tss_dtor) != thrd_success)
        return 60;
    tss_set(g_tss, (void *)(intptr_t)9);
    if (tss_get(g_tss) != (void *)(intptr_t)9)
        return 61;
    /* a DEADLINE: 30 ms from now, not 30 ms past 1970 */
    mtx_t m;
    cnd_t c;
    mtx_init(&m, mtx_timed);
    cnd_init(&c);
    struct timespec d = { 0, 0 }, t0;
    timespec_get(&d, TIME_UTC);
    d.tv_nsec += 30000000L;
    if (d.tv_nsec >= 1000000000L) {
        d.tv_sec++;
        d.tv_nsec -= 1000000000L;
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    mtx_lock(&m);
    int r = cnd_timedwait(&c, &m, &d);
    mtx_unlock(&m);
    long ms = elapsed_ms(&t0);
    if (r != thrd_timedout || (clock_ok && (ms < 20 || ms > 2000)))
        return 62;
    if (!threads_ok)
        return 0;
    thrd_t t;
    int res;
    if (thrd_create(&t, c11_worker, NULL) != thrd_success ||
        thrd_join(t, &res) != thrd_success || res != 5)
        return 63;
    if (thrd_create(&t, c11_worker, (void *)(intptr_t)1) != thrd_success ||
        thrd_join(t, &res) != thrd_success || res != 17)
        return 64;
    if (g_tss_dtor != 6 || tss_get(g_tss) != (void *)(intptr_t)9)
        return 65;
    return 0;
}

/* ---- fork's handlers: prepare in reverse, the others in order --------------- */

static char g_log[16];
static void log_c(char c) { size_t n = strlen(g_log); g_log[n] = c; }
static void p1(void) { log_c('a'); }
static void p2(void) { log_c('b'); }
static void pa1(void) { log_c('c'); }
static void pa2(void) { log_c('d'); }
static void ch1(void) { log_c('e'); }
static void ch2(void) { log_c('f'); }

static int atfork(void)
{
    pthread_atfork(p1, pa1, ch1);
    pthread_atfork(p2, pa2, ch2);
    fflush(stdout);
    pid_t p = fork();
    if (p < 0)
        return 0;                  /* no fork here: nothing to order */
    if (p == 0)
        _exit(strcmp(g_log, "baef") == 0 ? 0 : 1);
    int st;
    if (waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
        return 70;
    return strcmp(g_log, "bacd") == 0 ? 0 : 71;
}

int main(void)
{
    struct timespec t;
    clock_ok = clock_gettime(CLOCK_MONOTONIC, &t) == 0;
    int r;
    if ((r = basics()) || (r = mutexes()) || (r = conds()) || (r = others()) ||
        (r = sems()) || (r = c11()) || (r = atfork()))
        return r;
    return 42;
}
