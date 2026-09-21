/* <stop_token>, <syncstream>, <valarray>, <typeindex>, <execution>,
 * <iosfwd>, <scoped_allocator>.
 *
 * The headers that close the C++20 list. Three of them are substantial
 * and four are small, and the small ones are still worth a check
 * because "it compiled" is not the same as "it does the thing".
 *
 * The property each is tested for:
 *
 *   stop_token   that a callback registered AFTER the stop runs
 *     immediately rather than being dropped, and that destroying one
 *     before the stop deregisters it. Cancellation is where
 *     use-after-free lives, and those two cases are why.
 *
 *   syncstream   that nothing reaches the wrapped stream until the
 *     object is destroyed. Emitting early would make the block
 *     non-atomic and the whole type pointless.
 *
 *   valarray     that the comparisons give a MASK rather than a bool,
 *     and that a slice assignment writes THROUGH to the original.
 */
#include "check.h"
#include <execution>
#include <iosfwd>
#include <scoped_allocator>
#include <sstream>
#include <stop_token>
#include <string>
#include <syncstream>
#include <typeindex>
#include <unordered_map>
#include <valarray>
#include <vector>

using namespace std;

struct Base { virtual ~Base() = default; };
struct Derived : Base {};

int main()
{
    /* ---- stop_token ---------------------------------------------------- */
    {
        stop_source s;
        stop_token t = s.get_token();
        CHECK(!t.stop_requested());
        CHECK(t.stop_possible());
        CHECK(s.stop_possible());

        int fired = 0;
        {
            stop_callback cb(t, [&fired] { fired++; });
            CHECK(fired == 0);
            /* Only the FIRST request succeeds, and the return value
             * says which caller made it. */
            CHECK(s.request_stop());
            CHECK(fired == 1);
            CHECK(!s.request_stop());
            CHECK(fired == 1);
        }
        CHECK(t.stop_requested());
        CHECK(s.stop_requested());
    }

    /* A callback registered AFTER the stop runs IMMEDIATELY. Dropping
     * it silently is the bug this checks for. */
    {
        stop_source s;
        stop_token t = s.get_token();
        s.request_stop();
        int fired = 0;
        stop_callback cb(t, [&fired] { fired++; });
        CHECK(fired == 1);
    }

    /* One destroyed BEFORE the stop must not run -- which is the
     * guarantee that makes it safe to capture a local. */
    {
        stop_source s;
        stop_token t = s.get_token();
        int fired = 0;
        {
            stop_callback cb(t, [&fired] { fired++; });
        }
        s.request_stop();
        CHECK(fired == 0);
    }

    /* Several callbacks all run. */
    {
        stop_source s;
        stop_token t = s.get_token();
        int a = 0, b = 0, c = 0;
        stop_callback ca(t, [&a] { a = 1; });
        stop_callback cb(t, [&b] { b = 1; });
        stop_callback cc(t, [&c] { c = 1; });
        s.request_stop();
        CHECK(a == 1 && b == 1 && c == 1);
    }

    /* A token from a copied source shares the state; one from a
     * default-constructed token shares nothing. */
    {
        stop_source a;
        stop_source b = a;
        CHECK(a == b);
        CHECK(a.get_token() == b.get_token());
        b.request_stop();
        CHECK(a.stop_requested());

        stop_token empty;
        CHECK(!empty.stop_requested());
        /* Nothing can ever stop it, and saying so lets a worker take
         * the fast path. */
        CHECK(!empty.stop_possible());
        stop_source none{nostopstate};
        CHECK(!none.stop_possible());
        CHECK(!none.request_stop());
    }

    /* A token outliving every source cannot be stopped any more. */
    {
        stop_token t;
        {
            stop_source s;
            t = s.get_token();
            CHECK(t.stop_possible());
        }
        CHECK(!t.stop_possible());
        CHECK(!t.stop_requested());
    }

    /* ---- jthread: the failure is honest on a target with no threads --- */
    {
        jthread j;
        CHECK(!j.joinable());
        CHECK(!j.get_stop_token().stop_requested());
        /* Its stop_source works whether or not a thread exists. */
        CHECK(j.request_stop());
        CHECK(j.get_stop_token().stop_requested());

        bool started = false, threw = false;
        try {
            jthread k([](stop_token st) { (void)st; });
            started = true;
        } catch (const system_error &) {
            threw = true;
        }
        CHECK(started || threw);
    }

    /* ---- syncstream ----------------------------------------------------- */
    {
        ostringstream out;
        {
            osyncstream s(out);
            s << "hello " << 42 << '!';
            /* NOTHING has reached the stream yet. Emitting early would
             * make the block non-atomic, which is the whole reason the
             * type exists. */
            CHECK(out.str().empty());
        }
        CHECK(out.str() == "hello 42!");
    }

    /* emit() flushes without waiting for the destructor. */
    {
        ostringstream out;
        osyncstream s(out);
        s << "part";
        CHECK(out.str().empty());
        s.emit();
        CHECK(out.str() == "part");
        s << "more";
        CHECK(out.str() == "part");
        s.emit();
        CHECK(out.str() == "partmore");
    }

    /* Two blocks land whole, in the order they were emitted -- never
     * interleaved, which is the property a shared cout does not have. */
    {
        ostringstream out;
        {
            osyncstream a(out);
            a << "AAAA";
            {
                osyncstream b(out);
                b << "BBBB";
            }
            /* b emitted on destruction; a has still written nothing. */
            CHECK(out.str() == "BBBB");
        }
        CHECK(out.str() == "BBBBAAAA");
    }

    {
        ostringstream out;
        osyncstream s(out);
        CHECK(s.get_wrapped() == out.rdbuf());
    }

    /* ---- valarray -------------------------------------------------------- */
    {
        valarray<int> a{1, 2, 3, 4};
        CHECK(a.size() == 4);
        CHECK(a[0] == 1 && a[3] == 4);

        /* Elementwise, which is what the type is for. */
        valarray<int> b = a * 2;
        CHECK(b[0] == 2 && b[3] == 8);
        valarray<int> c = a + b;
        CHECK(c[0] == 3 && c[3] == 12);
        valarray<int> d = -a;
        CHECK(d[0] == -1);
        CHECK((a * 2).sum() == 20);

        valarray<int> e = a;
        e += 10;
        CHECK(e[0] == 11 && e[3] == 14);
        e -= a;
        CHECK(e[0] == 10 && e[3] == 10);

        CHECK(a.sum() == 10);
        CHECK(a.min() == 1 && a.max() == 4);

        /* Assigning a scalar FILLS. */
        valarray<int> f(5);
        f = 7;
        CHECK(f[0] == 7 && f[4] == 7 && f.size() == 5);

        /* Assigning a valarray of another size RESIZES, unlike almost
         * everything else here. */
        valarray<int> g;
        g = a;
        CHECK(g.size() == 4 && g[2] == 3);
    }

    /* The comparisons give a MASK, elementwise -- not a single bool.
     * `if (a == b)` does not compile, which is the design refusing to
     * guess whether "equal" meant all or any. */
    {
        valarray<int> a{1, 2, 3, 4};
        valarray<bool> m = a > 2;
        CHECK(m.size() == 4);
        CHECK(!m[0] && !m[1] && m[2] && m[3]);
        static_assert(is_same_v<decltype(a > 2), valarray<bool>>);
        valarray<bool> eq = (a == a);
        CHECK(eq[0] && eq[3]);
    }

    /* A slice is start, count, stride -- three of them two apart, NOT
     * "from 1 to 3 by 2". */
    {
        valarray<int> a{0, 1, 2, 3, 4, 5};
        valarray<int> s = a[slice(1, 3, 2)];
        CHECK(s.size() == 3);
        CHECK(s[0] == 1 && s[1] == 3 && s[2] == 5);

        /* Assigning to a slice writes THROUGH to the original. */
        a[slice(0, 3, 2)] = 99;
        CHECK(a[0] == 99 && a[2] == 99 && a[4] == 99);
        CHECK(a[1] == 1 && a[3] == 3 && a[5] == 5);

        valarray<int> b{10, 20, 30, 40, 50, 60};
        valarray<int> src{7, 7, 7};
        b[slice(0, 3, 2)] = src;
        CHECK(b[0] == 7 && b[2] == 7 && b[4] == 7 && b[1] == 20);
    }

    /* shift fills with zero; cshift wraps. A negative count goes the
     * other way, which is the half that is easy to get wrong. */
    {
        valarray<int> a{1, 2, 3, 4};
        valarray<int> s = a.shift(1);
        CHECK(s[0] == 2 && s[2] == 4 && s[3] == 0);
        valarray<int> sb = a.shift(-1);
        CHECK(sb[0] == 0 && sb[1] == 1 && sb[3] == 3);
        valarray<int> c = a.cshift(1);
        CHECK(c[0] == 2 && c[3] == 1);
        valarray<int> cb = a.cshift(-1);
        CHECK(cb[0] == 4 && cb[1] == 1);
    }

    /* The transcendental functions, elementwise. */
    {
        valarray<double> a{0.0, 1.0, 4.0, 9.0};
        valarray<double> r = sqrt(a);
        CHECK(r[0] == 0.0 && r[1] == 1.0 && r[2] == 2.0 && r[3] == 3.0);
        valarray<double> e = exp(valarray<double>{0.0, 0.0});
        CHECK(e[0] == 1.0 && e[1] == 1.0);
        valarray<double> p = pow(valarray<double>{2.0, 3.0}, 2.0);
        CHECK(p[0] == 4.0 && p[1] == 9.0);
        valarray<int> ab = abs(valarray<int>{-1, 2, -3});
        CHECK(ab[0] == 1 && ab[1] == 2 && ab[2] == 3);
    }

    /* apply, and the range access that makes a valarray usable with
     * the ordinary algorithms. */
    {
        valarray<int> a{1, 2, 3};
        valarray<int> b = a.apply([](int x) { return x * x; });
        CHECK(b[0] == 1 && b[1] == 4 && b[2] == 9);
        int total = 0;
        for (int x : a)
            total += x;
        CHECK(total == 6);
        CHECK(end(a) - begin(a) == 3);
    }

    /* resize DISCARDS -- it does not preserve, the way a vector's
     * does. The name is the trap. */
    {
        valarray<int> a{1, 2, 3};
        a.resize(5, 8);
        CHECK(a.size() == 5);
        CHECK(a[0] == 8 && a[4] == 8);
    }

    /* ---- typeindex -------------------------------------------------------- */
    {
        type_index a(typeid(int));
        type_index b(typeid(int));
        type_index c(typeid(double));
        CHECK(a == b);
        CHECK(a != c);
        CHECK(a.hash_code() == b.hash_code());
        CHECK(string(a.name()).size() > 0);
        /* The order is unspecified but CONSISTENT, which is all a map
         * needs -- and is why nothing may be persisted that depends on
         * it. */
        CHECK((a < c) != (c < a));
        CHECK(!(a < b) && !(b < a));

        /* The shape of every plugin registry: a map keyed on a type. */
        unordered_map<type_index, string> names;
        names[type_index(typeid(int))] = "int";
        names[type_index(typeid(double))] = "double";
        names[type_index(typeid(Derived))] = "Derived";
        CHECK(names.size() == 3);
        CHECK(names[type_index(typeid(int))] == "int");

        /* Through a base pointer it is the DYNAMIC type, which is the
         * reason the key is a type_info rather than a template
         * parameter. */
        Derived d;
        Base *p = &d;
        CHECK(names[type_index(typeid(*p))] == "Derived");
    }

    /* ---- execution -------------------------------------------------------- */
    {
        static_assert(is_execution_policy_v<execution::sequenced_policy>);
        static_assert(is_execution_policy_v<execution::parallel_policy>);
        static_assert(
            is_execution_policy_v<execution::parallel_unsequenced_policy>);
        static_assert(!is_execution_policy_v<int>);
        /* They are types, not flags, so the choice is made by overload
         * resolution and an algorithm that cannot honour one simply
         * does not accept it. */
        static_assert(!is_same_v<decltype(execution::seq),
                                 decltype(execution::par)>);
        (void)execution::seq;
        (void)execution::par;
        (void)execution::par_unseq;
        (void)execution::unseq;
    }

    /* ---- iosfwd: a declaration is enough to name a type ------------------- */
    {
        /* The point of the header is that a reference to an INCOMPLETE
         * type is enough to declare a function -- which is what keeps
         * <ostream> out of headers that only mention it. */
        ostream *p = nullptr;
        CHECK(p == nullptr);
        static_assert(is_same_v<streamsize, ptrdiff_t>);
    }

    /* ---- scoped_allocator: the uses-allocator protocol -------------------- */
    {
        /* A container says it takes an allocator by declaring
         * allocator_type, and that is what the adaptor detects. */
        static_assert(uses_allocator_v<vector<int>, allocator<int>>);
        static_assert(!uses_allocator_v<int, allocator<int>>);
        scoped_allocator_adaptor<allocator<int>> a;
        int *p = a.allocate(4);
        a.construct(p, 42);
        CHECK(p[0] == 42);
        a.destroy(p);
        a.deallocate(p, 4);
    }

    DONE();
}
