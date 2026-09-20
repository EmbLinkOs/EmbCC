/* <atomic>.
 *
 * There is one thread here, so this cannot test for a race. What it CAN
 * test is everything else, and that is most of what goes wrong:
 *
 *   the VALUE each operation returns. fetch_add returns the value
 *     *before* the addition and ++ returns the one *after*, and getting
 *     that backwards makes a ticket dispenser hand out the same number
 *     twice -- a bug that needs two threads to bite and one to write.
 *
 *   compare_exchange's contract on FAILURE. It writes back what was
 *     actually there, which is the whole reason the argument is a
 *     reference and the whole reason a CAS loop terminates.
 *
 *   that the memory orders reach the compiler at all. Each is exercised
 *     so that a target whose lowering is missing one fails here rather
 *     than in the kernel.
 *
 * The one thing a single thread proves about concurrency is that these
 * are the right INSTRUCTIONS: a seq_cst store must be an exchange on
 * x86-64 and a store-release on aarch64, and the golden below runs on
 * both.
 */
#include "check.h"
#include <atomic>
#include <cstdint>

using namespace std;

struct Pair { int a, b; };          /* trivially copyable, two words */

int main()
{
    /* ---- construction, load, store ------------------------------------ */
    {
        atomic<int> a{7};
        CHECK(a.load() == 7);
        CHECK(a == 7);                      /* the conversion operator */
        a.store(9);
        CHECK(a.load() == 9);
        /* Assignment yields the VALUE, not a reference: returning *this
         * would let `x = a = 5` read `a` again, and by then another
         * thread may have changed it. */
        int got = (a = 3);
        CHECK(got == 3 && a.load() == 3);

        atomic<int> b;                      /* default: no initialization */
        b.store(0);
        CHECK(b.load() == 0);
    }

    /* ---- every memory order reaches the compiler ----------------------- */
    {
        atomic<int> a{0};
        a.store(1, memory_order_relaxed);
        CHECK(a.load(memory_order_relaxed) == 1);
        a.store(2, memory_order_release);
        CHECK(a.load(memory_order_acquire) == 2);
        a.store(3, memory_order_seq_cst);
        CHECK(a.load(memory_order_seq_cst) == 3);
        CHECK(a.load(memory_order_consume) == 3);
        CHECK(a.exchange(4, memory_order_acq_rel) == 3);
        atomic_thread_fence(memory_order_acquire);
        atomic_thread_fence(memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);
        /* A signal fence emits no instruction: it stops the COMPILER
         * reordering, not the core, because a handler runs on the same
         * core. It must still compile. */
        atomic_signal_fence(memory_order_seq_cst);
        CHECK(a.load() == 4);
    }

    /* ---- exchange returns the OLD value -------------------------------- */
    {
        atomic<int> a{10};
        CHECK(a.exchange(20) == 10);
        CHECK(a.load() == 20);
    }

    /* ---- compare_exchange, both outcomes -------------------------------- */
    {
        atomic<int> a{5};
        int expected = 5;
        CHECK(a.compare_exchange_strong(expected, 6));
        CHECK(a.load() == 6);
        CHECK(expected == 5);            /* unchanged on success */

        expected = 99;                   /* wrong: the exchange must fail */
        CHECK(!a.compare_exchange_strong(expected, 7));
        CHECK(a.load() == 6);            /* ... and store nothing */
        /* The contract that makes a CAS loop terminate: on failure
         * `expected` is overwritten with what was ACTUALLY there, so the
         * next iteration has the current value and does not spin on a
         * stale one forever. */
        CHECK(expected == 6);

        /* Which is exactly how one is written. */
        atomic<int> c{1};
        int old = c.load();
        while (!c.compare_exchange_weak(old, old * 10))
            ;                            /* old is refreshed by the call */
        CHECK(c.load() == 10);

        /* The weak form may fail spuriously, so its single-shot use is a
         * bug; its loop use is the cheap one on a load-linked machine.
         * Here only that it compiles and eventually succeeds. */
        atomic<int> w{0};
        int we = 0;
        int spins = 0;
        while (!w.compare_exchange_weak(we, 1) && spins < 1000)
            spins++;
        CHECK(w.load() == 1);

        /* Explicit success and failure orders, including the pair the
         * standard restricts: a failed exchange performs no store, so
         * its order may not be release. */
        atomic<int> d{1};
        int de = 1;
        CHECK(d.compare_exchange_strong(de, 2, memory_order_acq_rel,
                                        memory_order_acquire));
        de = 1;
        CHECK(!d.compare_exchange_strong(de, 3, memory_order_release,
                                         memory_order_relaxed));
        CHECK(de == 2);
    }

    /* ---- fetch_* return BEFORE, operators return AFTER ------------------ */
    {
        atomic<int> a{10};
        CHECK(a.fetch_add(5) == 10 && a.load() == 15);
        CHECK(a.fetch_sub(3) == 15 && a.load() == 12);
        CHECK(a.fetch_and(0xC) == 12 && a.load() == 12);
        CHECK(a.fetch_or(1) == 12 && a.load() == 13);
        CHECK(a.fetch_xor(0xF) == 13 && a.load() == 2);

        a.store(10);
        CHECK(++a == 11);                /* the NEW value */
        CHECK(a++ == 11);                /* the OLD value */
        CHECK(a.load() == 12);
        CHECK(--a == 11);
        CHECK(a-- == 11);
        CHECK(a.load() == 10);
        CHECK((a += 5) == 15);
        CHECK((a -= 2) == 13);
        CHECK((a &= 0xC) == 12);
        CHECK((a |= 3) == 15);
        CHECK((a ^= 0xF) == 0);
    }

    /* A ticket dispenser is the reason fetch_add returns the old value:
     * every caller gets a different number and none is skipped. */
    {
        atomic<int> next{0};
        int t[5];
        for (int i = 0; i < 5; i++)
            t[i] = next.fetch_add(1);
        CHECK(t[0] == 0 && t[4] == 4 && next.load() == 5);
    }

    /* ---- the widths, signed and unsigned -------------------------------- */
    {
        atomic<unsigned char> c{250};
        CHECK(c.fetch_add(10) == 250);
        CHECK(c.load() == 4);            /* unsigned wraps, and must */

        atomic<int64_t> big{0};
        big.store(int64_t(1) << 40);
        CHECK(big.load() == (int64_t(1) << 40));
        CHECK(big.fetch_add(1) == (int64_t(1) << 40));

        atomic<uint64_t> u{0};
        u.store(~uint64_t(0));
        CHECK(u.load() == ~uint64_t(0));
        CHECK(u.fetch_add(1) == ~uint64_t(0));
        CHECK(u.load() == 0);

        atomic<short> s{-1};
        CHECK(s.load() == -1);
        CHECK(s.fetch_sub(1) == -1 && s.load() == -2);

        atomic<bool> f{false};
        CHECK(!f.load());
        f.store(true);
        CHECK(f.load());
        CHECK(f.exchange(false));
        CHECK(!f.load());
    }

    /* ---- lock-freedom --------------------------------------------------- */
    {
        /* Every type that fits a word on both targets here is lock-free,
         * and `is_always_lock_free` is a CONSTANT -- so code can choose
         * an algorithm at compile time rather than testing at run time. */
        static_assert(atomic<int>::is_always_lock_free);
        static_assert(atomic<void *>::is_always_lock_free);
        static_assert(atomic<char>::is_always_lock_free);
        atomic<int> a{0};
        CHECK(a.is_lock_free());
    }

    /* ---- atomic_flag: the one type guaranteed lock-free everywhere ------ */
    {
        atomic_flag f = ATOMIC_FLAG_INIT;
        /* test_and_set returns what it was, so the FIRST caller sees
         * false -- which is what makes it a lock. */
        CHECK(!f.test_and_set());
        CHECK(f.test_and_set());         /* already held */
        CHECK(f.test());
        f.clear();
        CHECK(!f.test());
        CHECK(!f.test_and_set());

        /* A spin lock, written out, because the type exists for exactly
         * this and nothing else. */
        atomic_flag lock = ATOMIC_FLAG_INIT;
        int guarded = 0;
        for (int i = 0; i < 3; i++) {
            while (lock.test_and_set(memory_order_acquire))
                ;
            guarded++;
            lock.clear(memory_order_release);
        }
        CHECK(guarded == 3);
    }

    /* ---- pointers: arithmetic in ELEMENTS ------------------------------- */
    {
        int arr[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        atomic<int *> p{arr};
        CHECK(p.load() == arr);
        /* fetch_add(2) moves two INTS, not two bytes -- the builtin
         * takes bytes and the scaling is the library's job. */
        CHECK(p.fetch_add(2) == arr);
        CHECK(p.load() == arr + 2);
        CHECK(*p.load() == 2);
        CHECK(p.fetch_sub(1) == arr + 2);
        CHECK(p.load() == arr + 1);
        CHECK(++p == arr + 2);
        CHECK(p++ == arr + 2);
        CHECK(p.load() == arr + 3);
        CHECK((p += 4) == arr + 7);
        CHECK((p -= 3) == arr + 4);

        int *ex = arr + 4;
        CHECK(p.compare_exchange_strong(ex, arr));
        CHECK(p.load() == arr);
    }

    /* ---- a small trivially copyable struct ------------------------------ */
    {
        /* The generic path: load, store, exchange and compare_exchange
         * work on any trivially copyable type by copying its bytes.
         * compare_exchange compares the OBJECT REPRESENTATION, not
         * operator==, which is why a type with padding is a trap and
         * this one has none. */
        atomic<Pair> a{Pair{1, 2}};
        Pair got = a.load();
        CHECK(got.a == 1 && got.b == 2);
        a.store(Pair{3, 4});
        got = a.load();
        CHECK(got.a == 3 && got.b == 4);
        Pair old = a.exchange(Pair{5, 6});
        CHECK(old.a == 3 && old.b == 4);
        Pair exp{5, 6};
        CHECK(a.compare_exchange_strong(exp, Pair{7, 8}));
        CHECK(a.load().a == 7);
        exp = Pair{0, 0};
        CHECK(!a.compare_exchange_strong(exp, Pair{9, 9}));
        CHECK(exp.a == 7 && exp.b == 8);   /* refreshed on failure */
    }

    /* ---- atomic_ref: the same operations on someone else's object ------- */
    {
        /* The case atomic<T> cannot cover: an array shared between
         * threads, or a field whose layout an ABI fixed. */
        int shared[3] = {1, 2, 3};
        atomic_ref<int> r(shared[1]);
        CHECK(r.load() == 2);
        r.store(20);
        CHECK(shared[1] == 20);            /* it writes THROUGH */
        CHECK(r.fetch_add(5) == 20);
        CHECK(shared[1] == 25);
        CHECK(r.exchange(0) == 25);
        int e = 0;
        CHECK(r.compare_exchange_strong(e, 42));
        CHECK(shared[1] == 42);
        CHECK(shared[0] == 1 && shared[2] == 3);   /* neighbours untouched */
        static_assert(atomic_ref<int>::is_always_lock_free);
    }

    /* ---- the free-function forms ----------------------------------------- */
    {
        atomic<int> a{1};
        atomic_store(&a, 5);
        CHECK(atomic_load(&a) == 5);
        CHECK(atomic_exchange(&a, 6) == 5);
        int e = 6;
        CHECK(atomic_compare_exchange_strong(&a, &e, 7));
        CHECK(atomic_fetch_add(&a, 3) == 7);
        CHECK(atomic_load_explicit(&a, memory_order_acquire) == 10);
        atomic_store_explicit(&a, 0, memory_order_release);
        CHECK(a.load() == 0);

        atomic_flag f = ATOMIC_FLAG_INIT;
        CHECK(!atomic_flag_test_and_set(&f));
        atomic_flag_clear(&f);
        CHECK(!atomic_flag_test_and_set_explicit(&f, memory_order_acquire));
        atomic_flag_clear_explicit(&f, memory_order_release);
    }

    /* ---- an atomic is not copyable, and that is deliberate --------------- */
    static_assert(!is_copy_constructible_v<atomic<int>>);
    static_assert(!is_copy_assignable_v<atomic<int>>);
    /* Copying one would have to read it and write another, which is two
     * operations and therefore not atomic -- there is no instruction for
     * it, so the standard deletes the constructor rather than pretend. */

    DONE();
}
