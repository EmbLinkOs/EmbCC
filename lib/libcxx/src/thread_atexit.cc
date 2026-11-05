/* __cxa_thread_atexit (Itanium 3.3.6.3): a thread_local object's
 * destructor, run when the thread that made the object ends -- in the
 * reverse order of the objects' construction, as a thread's own atexit.
 *
 * Each thread keeps its list in a thread-local, and a pthread key whose
 * destructor runs it (a key's destructor runs while the thread's
 * thread-locals are still there). The main thread ends in exit(), which
 * runs no key destructors, so the list of whichever thread calls exit()
 * is run from __cxa_atexit as well. Weak: a system runtime's (libstdc++'s,
 * libc++abi's) wins where one is linked.
 */
#include <pthread.h>
#include <stdlib.h>

extern "C" {

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso);

struct __cx_tdtor {
    void (*fn)(void *);
    void *obj;
    __cx_tdtor *next;
};

static __thread __cx_tdtor *g_list;
static pthread_key_t g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_have_key, g_at_exit;

static void run_list(void *)
{
    while (__cx_tdtor *d = g_list) {
        g_list = d->next;
        d->fn(d->obj);
        free(d);
    }
}

static void make_key()
{
    g_have_key = pthread_key_create(&g_key, run_list) == 0;
}

__attribute__((weak)) int __cxa_thread_atexit(void (*fn)(void *), void *obj, void *dso)
{
    pthread_once(&g_once, make_key);
    __cx_tdtor *d = (__cx_tdtor *)malloc(sizeof *d);
    if (!d)
        return -1;
    d->fn = fn;
    d->obj = obj;
    d->next = g_list;
    if (!g_list && g_have_key)
        pthread_setspecific(g_key, (void *)1);   /* non-null: the key fires */
    g_list = d;
    if (!__atomic_exchange_n(&g_at_exit, 1, __ATOMIC_ACQ_REL))
        __cxa_atexit(run_list, 0, dso);
    return 0;
}

}  // extern "C"
