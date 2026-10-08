/* The memory orders an eight-byte atomic passes, seen from the callee:
 * this program defines its own __atomic_*_8, which replace lib/rt's weak
 * ones (as a multi-core part's SDK does), and each prints which routine
 * was called and the orders it was given -- the C __ATOMIC_* values, the
 * __sync builtins and the _Atomic operators seq_cst (5), and
 * __sync_lock_release release (3), as clang passes them. The expected
 * output is order.want (tests/golden/atomic8.sh). */
#include <stdatomic.h>

void writec(int c);
void puts_(const char *s);
void putn(long v);

typedef unsigned long long u64;

static void rec(const char *name, int o, int f)
{
    puts_(name);
    writec(' ');
    putn(o);
    if (f >= 0)
        putn(f);
    writec('\n');
}

u64 __atomic_load_8(const volatile void *p, int o)
{ rec("load", o, -1); return *(const volatile u64 *)p; }
void __atomic_store_8(volatile void *p, u64 v, int o)
{ rec("store", o, -1); *(volatile u64 *)p = v; }
u64 __atomic_exchange_8(volatile void *p, u64 v, int o)
{ u64 r = *(volatile u64 *)p; rec("exchange", o, -1); *(volatile u64 *)p = v; return r; }
_Bool __atomic_compare_exchange_8(volatile void *p, void *e, u64 v, int s, int f)
{
    volatile u64 *q = (volatile u64 *)p;
    rec("compare_exchange", s, f);
    if (*q == *(u64 *)e) { *q = v; return 1; }
    *(u64 *)e = *q;
    return 0;
}
#define OP(N, E) \
    u64 __atomic_fetch_##N##_8(volatile void *p, u64 v, int o) \
    { volatile u64 *q = (volatile u64 *)p; u64 r = *q; rec("fetch_" #N, o, -1); *q = (E); return r; }
OP(add, r + v)
OP(sub, r - v)
OP(and, r & v)
OP(or, r | v)
OP(xor, r ^ v)
OP(nand, ~(r & v))

static u64 d;
static _Atomic long long ad;

int main(void)
{
    u64 e = 0;
    /* eight bytes on every target (a double is four on RX) */
    struct pair { int a, b; } x = { 1, 2 }, y = { 3, 4 };
    static struct pair df __attribute__((aligned(8)));
    __atomic_load_n(&d, __ATOMIC_RELAXED);
    __atomic_load_n(&d, __ATOMIC_CONSUME);
    __atomic_store_n(&d, 1, __ATOMIC_RELEASE);
    __atomic_exchange_n(&d, 2, __ATOMIC_ACQ_REL);
    __atomic_compare_exchange_n(&d, &e, 3, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE);
    __atomic_compare_exchange_n(&d, &e, 3, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    __atomic_fetch_add(&d, 1, __ATOMIC_ACQUIRE);
    __atomic_fetch_sub(&d, 1, __ATOMIC_RELEASE);
    __atomic_fetch_and(&d, 1, __ATOMIC_RELAXED);
    __atomic_fetch_or(&d, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_xor(&d, 1, __ATOMIC_ACQ_REL);
    __atomic_fetch_nand(&d, 1, __ATOMIC_ACQUIRE);
    __atomic_sub_fetch(&d, 1, __ATOMIC_RELAXED);
    {
        volatile int o = __ATOMIC_ACQUIRE;       /* not a constant */
        __atomic_fetch_add(&d, 1, o);
    }
    __sync_fetch_and_add(&d, 1);
    __sync_val_compare_and_swap(&d, 1, 2);
    __sync_lock_test_and_set(&d, 1);
    __sync_lock_release(&d);
    __atomic_load(&df, &y, __ATOMIC_ACQUIRE);
    __atomic_store(&df, &x, __ATOMIC_RELEASE);
    __atomic_compare_exchange(&df, &x, &y, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    ad++;
    ad -= 2;
    ad *= 3;
    ad = 4;
    e = ad;
    atomic_fetch_add_explicit(&ad, 1, memory_order_release);
    puts_("DONE\n");
    return (int)e - 4;
}
