/* Eight-byte atomics on the 32-bit targets, which are calls to
 * libatomic's __atomic_*_8 (lib/rt/atomic8.c), run on each board and
 * compared with the host (tests/golden/atomic8.sh): the __atomic and
 * __sync builtins, the generic forms on a double and an eight-byte
 * struct, the operators on an _Atomic long long, and <stdatomic.h>'s
 * functions. Values are chosen so the halves differ and carries and
 * borrows cross between them: a call that passed the halves swapped or
 * lost one prints something else. */
#include <stdatomic.h>

void writec(int c);
void puts_(const char *s);
void putn(long v);

typedef unsigned long long u64;
typedef long long s64;

/* in 16-bit pieces, the same on every long size */
static void put64(u64 v)
{
    for (int k = 48; k >= 0; k -= 16)
        putn((long)((v >> k) & 0xffff));
}

static u64 d;
static s64 sd;
static double dd;
static struct pair { int a, b; } __attribute__((aligned(8))) pr;
static _Atomic s64 ad;
static _Atomic u64 au;

/* through arguments: the pointer and the value arrive in registers */
__attribute__((noinline)) u64 add_at(u64 *p, u64 v, int order)
{ return __atomic_fetch_add(p, v, order); }
__attribute__((noinline)) int cas_at(u64 *p, u64 *e, u64 v)
{ return __atomic_compare_exchange_n(p, e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED); }

static void builtins(void)
{
    __atomic_store_n(&d, 0x00000001ffffffffull, __ATOMIC_SEQ_CST);
    put64(__atomic_load_n(&d, __ATOMIC_ACQUIRE));
    put64(__atomic_fetch_add(&d, 1, __ATOMIC_SEQ_CST)); put64(d);
    put64(__atomic_add_fetch(&d, 0xffffffffull, __ATOMIC_RELAXED));
    put64(__atomic_fetch_sub(&d, 0x0000000100000001ull, __ATOMIC_SEQ_CST)); put64(d);
    put64(__atomic_sub_fetch(&d, 0x0000000300000000ull, __ATOMIC_ACQ_REL));
    put64(__atomic_fetch_and(&d, 0xf0f0f0f00f0f0f0full, __ATOMIC_SEQ_CST)); put64(d);
    put64(__atomic_and_fetch(&d, 0xff00ff00ff00ff00ull, __ATOMIC_SEQ_CST));
    put64(__atomic_fetch_or(&d, 0x0102030405060708ull, __ATOMIC_RELEASE)); put64(d);
    put64(__atomic_or_fetch(&d, 0x8000000000000001ull, __ATOMIC_SEQ_CST));
    put64(__atomic_fetch_xor(&d, 0xffffffff00000000ull, __ATOMIC_SEQ_CST)); put64(d);
    put64(__atomic_xor_fetch(&d, 0x00000000ffffffffull, __ATOMIC_SEQ_CST));
    put64(__atomic_fetch_nand(&d, 0x0000ffffffff0000ull, __ATOMIC_SEQ_CST)); put64(d);
    put64(__atomic_nand_fetch(&d, 0xffff00000000ffffull, __ATOMIC_SEQ_CST));
    put64(__atomic_exchange_n(&d, 0xdeadbeefcafef00dull, __ATOMIC_SEQ_CST)); put64(d);
    writec('\n');

    u64 e = 1;
    putn(__atomic_compare_exchange_n(&d, &e, 2, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    put64(e); put64(d);
    putn(__atomic_compare_exchange_n(&d, &e, 0x1122334455667788ull, 1,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    put64(e); put64(d);
    e = 0x1122334455667788ull;
    putn(cas_at(&d, &e, 0x8877665544332211ull)); put64(e); put64(d);
    put64(add_at(&d, 0x77889900aabbccddull, __ATOMIC_RELAXED)); put64(d);
    sd = -5;
    put64((u64)__atomic_fetch_add(&sd, -10, __ATOMIC_SEQ_CST)); put64((u64)sd);
    put64((u64)__atomic_fetch_sub(&sd, -0x100000000ll, __ATOMIC_SEQ_CST));
    put64((u64)sd);
    writec('\n');

    /* __sync: seq_cst throughout */
    d = 0x00000000fffffffeull;
    put64(__sync_fetch_and_add(&d, 3)); put64(d);
    put64(__sync_sub_and_fetch(&d, 2));
    put64(__sync_fetch_and_or(&d, 0xf000000000000000ull));
    put64(__sync_and_and_fetch(&d, 0xf0000000000000ffull));
    put64(__sync_fetch_and_xor(&d, 0x5555555555555555ull));
    put64(__sync_nand_and_fetch(&d, 0x00ff00ff00ff00ffull));
    put64(__sync_val_compare_and_swap(&d, 0, 7)); put64(d);
    put64(__sync_val_compare_and_swap(&d, d, 7)); put64(d);
    putn(__sync_bool_compare_and_swap(&d, 8, 9)); put64(d);
    putn(__sync_bool_compare_and_swap(&d, 7, 0x123456789ull)); put64(d);
    put64(__sync_lock_test_and_set(&d, 0xffffffffffffffffull)); put64(d);
    __sync_lock_release(&d); put64(d);
    writec('\n');
}

static void generic(void)
{
    /* a double and a struct move as their eight bytes */
    double v = 2.5, r = 0;
    __atomic_store(&dd, &v, __ATOMIC_SEQ_CST);
    __atomic_load(&dd, &r, __ATOMIC_SEQ_CST);
    putn((long)(r * 4));
    v = -0.75;
    __atomic_exchange(&dd, &v, &r, __ATOMIC_SEQ_CST);
    putn((long)(r * 4)); putn((long)(dd * 4));
    double ex = -0.75, de = 1e3;
    putn(__atomic_compare_exchange(&dd, &ex, &de, 0, __ATOMIC_SEQ_CST,
                                   __ATOMIC_SEQ_CST));
    putn((long)dd);
    ex = 7;
    putn(__atomic_compare_exchange(&dd, &ex, &de, 0, __ATOMIC_SEQ_CST,
                                   __ATOMIC_SEQ_CST));
    putn((long)ex);
    struct pair p = { -1, 77 }, q;
    __atomic_store(&pr, &p, __ATOMIC_SEQ_CST);
    __atomic_load(&pr, &q, __ATOMIC_SEQ_CST);
    putn(q.a); putn(q.b);
    writec('\n');
}

static void operators(void)
{
    ad = 0x7fffffff;
    putn((long)(ad++ & 0xffff)); put64((u64)ad);
    put64((u64)++ad); put64((u64)ad--); put64((u64)--ad);
    put64((u64)(ad += 0x100000000ll)); put64((u64)(ad -= 0x1ffffffffll));
    put64((u64)(ad |= 0x4000000000000000ll)); put64((u64)(ad &= ~0xffll));
    put64((u64)(ad ^= -1)); put64((u64)(ad *= 3)); put64((u64)(ad /= -7));
    put64((u64)(ad %= 1000003));
    au = 0x123456789ull;
    put64(au *= 0x10003); put64(au <<= 5); put64(au >>= 9);
    s64 x = ad;
    put64((u64)x);
    ad = -123456789012ll;
    put64((u64)ad);
    au = 5;
    put64(au -= 7); put64(au--); put64(au);
    writec('\n');
}

static void c11(void)
{
    atomic_ullong a = 10;
    put64(atomic_fetch_add(&a, 0xfffffffffull));
    put64(atomic_fetch_sub_explicit(&a, 1, memory_order_relaxed));
    put64(atomic_fetch_or_explicit(&a, 1ull << 40, memory_order_acquire));
    put64(atomic_fetch_and(&a, ~(1ull << 3)));
    put64(atomic_fetch_xor_explicit(&a, 0x0f0f, memory_order_release));
    put64(atomic_exchange(&a, 99));
    unsigned long long e = 98;
    putn(atomic_compare_exchange_strong(&a, &e, 100)); put64(e);
    putn(atomic_compare_exchange_weak_explicit(&a, &e, 100, memory_order_acq_rel,
                                               memory_order_acquire));
    put64(atomic_load(&a));
    atomic_store_explicit(&a, 0xa5a5a5a5a5a5a5a5ull, memory_order_release);
    put64(atomic_load_explicit(&a, memory_order_acquire));
    writec('\n');
}

int main(void)
{
    builtins();
    generic();
    operators();
    c11();
    puts_("DONE\n");
    return 0;
}
