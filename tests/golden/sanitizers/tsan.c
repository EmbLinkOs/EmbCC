#include <pthread.h>
#include <stdio.h>
int shared, guarded;
pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
_Atomic int flag;
int data;
void *racer(void *a) { (void)a; shared++; return 0; }
void *locker(void *a) { (void)a; pthread_mutex_lock(&mu); guarded++; pthread_mutex_unlock(&mu); return 0; }
void *producer(void *a) { (void)a; data = 42; __atomic_store_n(&flag, 1, __ATOMIC_RELEASE); return 0; }
void *consumer(void *a) { (void)a; while (!__atomic_load_n(&flag, __ATOMIC_ACQUIRE)); return (void*)(long)data; }
int main(int argc, char **argv) {
    pthread_t t1, t2;
    int mode = argc > 1 ? argv[1][0] - '0' : 0;
    if (mode == 1) { pthread_create(&t1, 0, racer, 0); pthread_create(&t2, 0, racer, 0); }
    else if (mode == 2) { pthread_create(&t1, 0, producer, 0); pthread_create(&t2, 0, consumer, 0); }
    else { pthread_create(&t1, 0, locker, 0); pthread_create(&t2, 0, locker, 0); }
    pthread_join(t1, 0); pthread_join(t2, 0);
    printf("done %d %d\n", shared, guarded);
    return 0;
}
