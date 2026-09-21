/* <mutex>, <shared_mutex>, <condition_variable>, <thread>, <latch>,
 * <semaphore>, <barrier>, <future>.
 *
 * There is ONE thread here -- lib/libc/os/backend.h's thread primitives
 * return ENOSYS on this target -- so this cannot test for a race. What
 * it tests is everything else, and that turns out to be most of what
 * these types are:
 *
 *   the UNCONTENDED paths, which are the paths a correct program takes
 *     almost always. A mutex locked by one thread never enters the
 *     kernel; a semaphore with a spare count never sleeps; a latch that
 *     is already zero returns at once. All of that is exercised here
 *     and all of it is where the ordinary bugs live.
 *
 *   the ERRORS. Unlocking what is not held, retrieving a future twice,
 *     joining a thread that is not joinable: each is a logic error the
 *     standard requires to be reported, and each is silent in an
 *     implementation that only ever gets tested on the happy path.
 *
 *   the HONEST FAILURE. Creating a std::thread here throws
 *     system_error, and that is the answer the design commits to --
 *     the alternative, running the function on the calling thread and
 *     calling it a thread, deadlocks the first time anything joins from
 *     inside it.
 */
#include "check.h"
#include <chrono>
#include <condition_variable>
#include <future>
#include <latch>
#include <barrier>
#include <mutex>
#include <semaphore>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std;

struct Counted {
    static int live;
    int v;
    explicit Counted(int x) : v(x) { live++; }
    Counted(const Counted &o) : v(o.v) { live++; }
    ~Counted() { live--; }
};
int Counted::live;

int main()
{
    /* ---- mutex: the uncontended path ------------------------------- */
    {
        mutex m;
        CHECK(m.try_lock());
        /* Already held: a second try must fail rather than succeed and
         * quietly let two owners in. */
        CHECK(!m.try_lock());
        m.unlock();
        CHECK(m.try_lock());
        m.unlock();
        m.lock();
        m.unlock();
    }

    /* ---- lock_guard releases on every path out, including a throw --- */
    {
        mutex m;
        int threw = 0;
        try {
            lock_guard<mutex> g(m);
            throw runtime_error("out");
        } catch (const runtime_error &) {
            threw = 1;
        }
        CHECK(threw);
        /* The whole reason a guard exists: the unlock happened during
         * unwinding, so the mutex is free. */
        CHECK(m.try_lock());
        m.unlock();
    }

    /* ---- unique_lock: deferred, adopted, released, moved ------------ */
    {
        mutex m;
        unique_lock<mutex> u(m, defer_lock);
        CHECK(!u.owns_lock() && !u);
        u.lock();
        CHECK(u.owns_lock() && (bool)u);
        u.unlock();
        CHECK(!u.owns_lock());
        CHECK(u.try_lock());

        /* Moving transfers the ownership, and leaves the source with
         * nothing to unlock. */
        unique_lock<mutex> v(static_cast<unique_lock<mutex> &&>(u));
        CHECK(v.owns_lock() && !u.owns_lock());
        /* release() hands the mutex back STILL LOCKED: the caller takes
         * over, which is the point. */
        mutex *raw = v.release();
        CHECK(raw == &m && !v.owns_lock());
        CHECK(!m.try_lock());        /* still held */
        m.unlock();

        /* The errors the standard requires to be reported. */
        {
            unique_lock<mutex> e;
            int n = 0;
            try { e.unlock(); } catch (const system_error &) { n++; }
            try { e.lock(); }   catch (const system_error &) { n++; }
            CHECK(n == 2);
        }
        {
            unique_lock<mutex> d(m);
            int n = 0;
            try { d.lock(); } catch (const system_error &) { n++; }
            CHECK(n == 1);           /* locking what is already held */
        }
    }

    /* ---- scoped_lock, over one and over two ------------------------- */
    {
        mutex a, b;
        {
            scoped_lock<mutex> g(a);
            CHECK(!a.try_lock());
        }
        CHECK(a.try_lock());
        a.unlock();
        {
            /* Two at once, through the algorithm that cannot deadlock:
             * take one, TRY the other, and on failure drop everything.
             * Because a thread that fails releases what it holds, there
             * is no cycle to wait in. */
            scoped_lock<mutex, mutex> g(a, b);
            CHECK(!a.try_lock() && !b.try_lock());
        }
        CHECK(a.try_lock() && b.try_lock());
        a.unlock();
        b.unlock();
    }

    /* ---- recursive_mutex: the same thread may re-enter --------------- */
    {
        recursive_mutex r;
        r.lock();
        r.lock();                    /* a plain mutex would deadlock here */
        r.lock();
        CHECK(r.try_lock());
        r.unlock();
        r.unlock();
        r.unlock();
        r.unlock();
        /* Fully released only after the LAST unlock. */
        CHECK(r.try_lock());
        r.unlock();
    }

    /* ---- timed_mutex: a wait that gives up --------------------------- */
    {
        timed_mutex t;
        CHECK(t.try_lock_for(chrono::milliseconds(1)));
        t.unlock();
        CHECK(t.try_lock());
        /* Held, and the timed attempt must END rather than block. */
        CHECK(!t.try_lock_for(chrono::milliseconds(1)));
        t.unlock();
    }

    /* ---- call_once: exactly once, and a throw does not count ---------- */
    {
        once_flag f;
        int n = 0;
        call_once(f, [&n] { n++; });
        call_once(f, [&n] { n++; });
        call_once(f, [&n] { n++; });
        CHECK(n == 1);

        /* A failed initialization has not initialized anything, so the
         * next caller tries again. Getting this wrong leaves an object
         * half-built and every later caller believing it is ready. */
        once_flag g;
        int tries = 0;
        for (int i = 0; i < 3; i++) {
            try {
                call_once(g, [&tries] {
                    tries++;
                    if (tries < 3)
                        throw runtime_error("not yet");
                });
            } catch (const runtime_error &) {
            }
        }
        CHECK(tries == 3);
        int after = tries;
        call_once(g, [&tries] { tries++; });
        CHECK(tries == after);       /* it succeeded, so no more */

        /* With arguments, forwarded. */
        once_flag h;
        int got = 0;
        call_once(h, [](int a, int b, int *o) { *o = a + b; }, 40, 2, &got);
        CHECK(got == 42);
    }

    /* ---- shared_mutex: many readers, one writer ---------------------- */
    {
        shared_mutex s;
        CHECK(s.try_lock_shared());
        CHECK(s.try_lock_shared());  /* a second reader gets in */
        CHECK(s.try_lock_shared());
        /* ... and a writer does not, while any reader holds it. */
        CHECK(!s.try_lock());
        s.unlock_shared();
        s.unlock_shared();
        CHECK(!s.try_lock());        /* one reader left is still a reader */
        s.unlock_shared();
        CHECK(s.try_lock());         /* now it is free */
        /* A writer excludes readers too -- that is the whole point. */
        CHECK(!s.try_lock_shared());
        CHECK(!s.try_lock());
        s.unlock();
        CHECK(s.try_lock_shared());
        s.unlock_shared();
    }

    /* ---- shared_lock ------------------------------------------------- */
    {
        shared_mutex s;
        {
            shared_lock<shared_mutex> r(s);
            CHECK(r.owns_lock());
            shared_lock<shared_mutex> r2(s);
            CHECK(r2.owns_lock());
            CHECK(!s.try_lock());
        }
        CHECK(s.try_lock());
        s.unlock();

        shared_lock<shared_mutex> d(s, defer_lock);
        CHECK(!d.owns_lock());
        CHECK(d.try_lock());
        d.unlock();
        int n = 0;
        try { d.unlock(); } catch (const system_error &) { n++; }
        CHECK(n == 1);
    }

    /* ---- condition_variable: the parts one thread can reach ---------- */
    {
        mutex m;
        condition_variable cv;
        /* A predicate that is ALREADY true must not wait at all --
         * which is the case a hand-written `if (!p) wait();` gets right
         * and a hand-written `wait(); if (!p) ...` does not. */
        bool ready = true;
        unique_lock<mutex> lk(m);
        cv.wait(lk, [&ready] { return ready; });
        CHECK(lk.owns_lock());       /* wait re-acquires before returning */

        /* wait_for with a predicate returns the PREDICATE, so a caller
         * writes `if (!wait_for(...))` and means "gave up". */
        CHECK(cv.wait_for(lk, chrono::milliseconds(1),
                          [&ready] { return ready; }));
        ready = false;
        CHECK(!cv.wait_for(lk, chrono::milliseconds(1),
                           [&ready] { return ready; }));
        CHECK(lk.owns_lock());

        /* Waiting without the lock is a logic error, not undefined. */
        lk.unlock();
        int n = 0;
        try { cv.wait(lk); } catch (const system_error &) { n++; }
        CHECK(n == 1);
        /* notify with nobody waiting is a no-op, not a lost signal to
         * be found later: a condition variable has no memory. */
        cv.notify_one();
        cv.notify_all();
    }

    /* ---- thread: the honest failure on a target with no threads ------ */
    {
        thread t;
        CHECK(!t.joinable());
        CHECK(t.get_id() == thread::id());
        /* Joining a default-constructed thread is an error, not a
         * silent no-op. */
        int n = 0;
        try { t.join(); }   catch (const system_error &) { n++; }
        try { t.detach(); } catch (const system_error &) { n++; }
        CHECK(n == 2);

        /* And creating one reports why it could not. */
        bool created = false, reported = false;
        try {
            thread u([] {});
            created = true;
            u.join();
        } catch (const system_error &e) {
            reported = e.code() == errc::function_not_supported;
        }
        CHECK(created || reported);
        CHECK(!(created && reported));

        /* this_thread works regardless: there is one thread and it has
         * an id, a yield and a sleep. */
        CHECK(this_thread::get_id() == this_thread::get_id());
        this_thread::yield();
        this_thread::sleep_for(chrono::nanoseconds(1));
        this_thread::sleep_for(chrono::milliseconds(-1));   /* no wait */
    }

    /* ---- latch: one-shot, and already-zero returns at once ------------ */
    {
        latch l(3);
        CHECK(!l.try_wait());
        l.count_down();
        CHECK(!l.try_wait());
        l.count_down(2);
        CHECK(l.try_wait());
        l.wait();                    /* already zero: returns */
        /* It stays zero forever -- that is the whole difference from a
         * barrier, and it is why a late arrival at wait() is free. */
        CHECK(l.try_wait());

        latch zero(0);
        CHECK(zero.try_wait());
        zero.wait();

        latch one(1);
        one.arrive_and_wait();
        CHECK(one.try_wait());
    }

    /* ---- semaphore: a counter, not a mutex ---------------------------- */
    {
        counting_semaphore<4> s(2);
        CHECK(s.try_acquire());
        CHECK(s.try_acquire());
        CHECK(!s.try_acquire());     /* the count is a real bound */
        s.release();
        CHECK(s.try_acquire());
        s.release(2);
        CHECK(s.try_acquire() && s.try_acquire());
        CHECK(!s.try_acquire());
        s.release(2);
        s.acquire();                 /* a spare count never sleeps */
        s.acquire();

        /* A semaphore has no owner, so releasing what you did not
         * acquire is allowed -- which is exactly why it is a signal and
         * not a lock. */
        binary_semaphore b(0);
        CHECK(!b.try_acquire());
        b.release();
        CHECK(b.try_acquire());
        CHECK(!b.try_acquire_for(chrono::milliseconds(1)));
    }

    /* ---- barrier: one participant, so every arrival completes a round - */
    {
        int rounds = 0;
        barrier<> plain(1);
        plain.arrive_and_wait();
        plain.arrive_and_wait();

        /* The completion runs once per round, on the last arrival, and
         * BEFORE anybody is released. */
        struct Done {
            int *n;
            void operator()() const noexcept { (*n)++; }
        };
        barrier<Done> b(1, Done{&rounds});
        b.arrive_and_wait();
        CHECK(rounds == 1);
        b.arrive_and_wait();
        CHECK(rounds == 2);

        /* A token from a round already over does not wait. */
        barrier<> c(1);
        auto tok = c.arrive();
        c.wait(static_cast<barrier<>::arrival_token &&>(tok));
    }

    /* ---- promise and future ------------------------------------------- */
    {
        promise<int> p;
        future<int> f = p.get_future();
        CHECK(f.valid());
        p.set_value(42);
        CHECK(f.wait_for(chrono::seconds(0)) == future_status::ready);
        CHECK(f.get() == 42);
        /* get() MOVES the value out: there is exactly one, and the
         * future is spent. */
        CHECK(!f.valid());
        int n = 0;
        try { (void)f.get(); } catch (const future_error &) { n++; }
        CHECK(n == 1);
    }

    /* The exception travels instead of the value. This is the half a
     * plain result type cannot do, and the reason a future exists. */
    {
        promise<int> p;
        future<int> f = p.get_future();
        p.set_exception(make_exception_ptr(runtime_error("boom")));
        int caught = 0;
        try { (void)f.get(); }
        catch (const runtime_error &e) { caught = string(e.what()) == "boom"; }
        CHECK(caught);
    }

    /* A promise destroyed without a value is a BROKEN PROMISE. Without
     * this the waiter hangs forever, which is the worst failure mode a
     * concurrency library has. */
    {
        future<int> f;
        {
            promise<int> p;
            f = p.get_future();
        }
        int code = 0;
        try { (void)f.get(); }
        catch (const future_error &e)
        { code = e.code() == future_errc::broken_promise; }
        CHECK(code);
    }

    /* The errors a promise reports. */
    {
        promise<int> p;
        (void)p.get_future();
        int n = 0;
        try { (void)p.get_future(); } catch (const future_error &e)
        { n += e.code() == future_errc::future_already_retrieved; }
        p.set_value(1);
        try { p.set_value(2); } catch (const future_error &e)
        { n += e.code() == future_errc::promise_already_satisfied; }
        CHECK(n == 2);
    }

    /* void, which needs its own specialization and is easy to forget. */
    {
        promise<void> p;
        future<void> f = p.get_future();
        p.set_value();
        f.get();
        CHECK(!f.valid());
    }

    /* shared_future: many readers, a reference each. */
    {
        promise<int> p;
        shared_future<int> s = p.get_future().share();
        shared_future<int> t = s;
        p.set_value(7);
        CHECK(s.get() == 7);
        CHECK(t.get() == 7);
        CHECK(s.get() == 7);         /* and again: it is not moved out */
    }

    /* A value that owns something: it must be destroyed exactly once
     * however the future is used. */
    {
        Counted::live = 0;
        {
            promise<Counted> p;
            future<Counted> f = p.get_future();
            p.set_value(Counted(3));
            Counted c = f.get();
            CHECK(c.v == 3);
        }
        CHECK(Counted::live == 0);
    }

    /* ---- packaged_task ------------------------------------------------- */
    {
        packaged_task<int(int, int)> t([](int a, int b) { return a + b; });
        future<int> f = t.get_future();
        t(40, 2);
        CHECK(f.get() == 42);

        /* An exception from the call is STORED, not propagated to
         * whoever ran the task -- which is what lets the task run on a
         * thread with no caller to catch it. */
        packaged_task<int()> bad([]() -> int { throw runtime_error("x"); });
        future<int> g = bad.get_future();
        bad();                       /* does not throw here */
        int caught = 0;
        try { (void)g.get(); } catch (const runtime_error &) { caught = 1; }
        CHECK(caught);

        packaged_task<void(int *)> v([](int *o) { *o = 5; });
        future<void> vf = v.get_future();
        int out = 0;
        v(&out);
        vf.get();
        CHECK(out == 5);
    }

    /* ---- async -------------------------------------------------------- */
    {
        /* The default policy lets the implementation choose, and on a
         * target with no threads it runs deferred -- which is the
         * correct answer rather than a compromise. */
        future<int> f = async([] { return 21 * 2; });
        CHECK(f.get() == 42);

        future<int> g = async([](int a, int b) { return a * b; }, 6, 7);
        CHECK(g.get() == 42);

        future<void> v = async(launch::deferred, [] {});
        v.get();

        /* An exception crosses it. */
        future<int> e = async([]() -> int { throw runtime_error("async"); });
        int caught = 0;
        try { (void)e.get(); } catch (const runtime_error &) { caught = 1; }
        CHECK(caught);

        /* An explicit launch::async on a target with no threads throws,
         * because the caller asked for concurrency that is not there
         * and quietly serialising would be a lie. */
        bool ran = false, threw = false;
        try {
            future<int> a = async(launch::async, [&ran] { ran = true; return 1; });
            CHECK(a.get() == 1);
        } catch (const system_error &) {
            threw = true;
        }
        CHECK(ran || threw);
    }

    DONE();
}
