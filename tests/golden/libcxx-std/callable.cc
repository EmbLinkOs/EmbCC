/* <functional>'s erased half, <system_error>, and <exception>'s
 * exception_ptr.
 *
 * Three things that look unrelated and are the same idea twice over:
 * something whose TYPE is not known until run time, carried in a way
 * that still knows what to do with it.
 *
 *   std::function erases a callable's type but keeps its signature.
 *   std::error_code erases which errno namespace a number came from,
 *     and keeps it: {2, generic} is ENOENT and {2, something_else} is
 *     not, and they compare unequal though both hold 2.
 *   std::exception_ptr erases an exception's type and rethrows it
 *     EXACTLY -- which is the only way a failure crosses a thread
 *     boundary, because a worker has no caller to throw to.
 */
#include "check.h"
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace std;

static int twice(int x) { return x * 2; }
static int add(int a, int b) { return a + b; }

struct Obj {
    int v;
    int get() const { return v; }
    int plus(int x) const { return v + x; }
    int field;
};

/* A callable with state large enough to be forced onto the heap, so
 * both paths of the small-object optimization are exercised. */
struct Big {
    long a, b, c, d, e;
    static int copies;
    Big(long x) : a(x), b(x), c(x), d(x), e(x) {}
    Big(const Big &o) : a(o.a), b(o.b), c(o.c), d(o.d), e(o.e) { copies++; }
    int operator()(int x) const { return (int)(x + a); }
};
int Big::copies;

int main()
{
    /* ---- function: empty is a state, and calling it is an error ------ */
    {
        function<int(int)> f;
        CHECK(!f);
        CHECK(f == nullptr);
        int threw = 0;
        try { (void)f(1); } catch (const bad_function_call &) { threw = 1; }
        CHECK(threw);
        CHECK(f.target_type() == typeid(void));
    }

    /* ---- what it can hold -------------------------------------------- */
    {
        function<int(int)> f;
        f = twice;
        CHECK(f && f(21) == 42);
        /* target() answers only for the RIGHT type: it is a checked
         * downcast, not a reinterpret. */
        CHECK(f.target<int (*)(int)>() != nullptr);
        CHECK(*f.target<int (*)(int)>() == &twice);
        CHECK(f.target<int (*)(long)>() == nullptr);

        f = [](int x) { return x + 1; };
        CHECK(f(41) == 42);
        CHECK(f.target<int (*)(int)>() == nullptr);   /* not a pointer now */

        int cap = 10;
        f = [cap](int x) { return x + cap; };
        CHECK(f(32) == 42);

        /* A reference captured by reference still refers. */
        int side = 0;
        function<void(int)> v = [&side](int x) { side = x; };
        v(7);
        CHECK(side == 7);

        /* A member function pointer is callable through a function
         * whose first parameter is the object. */
        function<int(const Obj &)> m = &Obj::get;
        Obj o{5, 0};
        CHECK(m(o) == 5);
        function<int(const Obj &, int)> m2 = &Obj::plus;
        CHECK(m2(o, 3) == 8);
        /* ... and so is a DATA member pointer, which reads it. */
        o.field = 9;
        function<int(const Obj &)> d = &Obj::field;
        CHECK(d(o) == 9);
    }

    /* ---- copy, move, and the value that goes on the heap ------------- */
    {
        Big::copies = 0;
        function<int(int)> f = Big(2);
        CHECK(f(40) == 42);
        function<int(int)> g = f;        /* copies the stored Big */
        CHECK(g(40) == 42);
        CHECK(Big::copies >= 1);
        function<int(int)> h = static_cast<function<int(int)> &&>(g);
        CHECK(h(40) == 42);
        /* A moved-from function is EMPTY, not a second reference to the
         * same target. */
        CHECK(!g);
        CHECK((bool)f);

        f.swap(h);
        CHECK(f(40) == 42 && h(40) == 42);
        f = nullptr;
        CHECK(!f && f == nullptr);
    }

    /* ---- the reason it exists: a container of them -------------------- */
    {
        using namespace placeholders;
        vector<function<int(int)>> ops;
        ops.push_back(twice);
        ops.push_back([](int x) { return x + 1; });
        ops.push_back(bind(add, 5, _1));
        ops.push_back(Big(100));
        int sum = 0;
        for (const auto &op : ops)
            sum += op(10);
        CHECK(sum == 20 + 11 + 15 + 110);
        /* Each kept its own type; the vector knows only the signature. */
        CHECK(ops[0].target_type() != ops[1].target_type());
    }

    /* ---- invoke: the four spellings of "call it" ---------------------- */
    {
        Obj o{5, 0};
        CHECK(invoke(twice, 21) == 42);
        CHECK(invoke(&Obj::get, o) == 5);
        CHECK(invoke(&Obj::get, &o) == 5);
        CHECK(invoke(&Obj::plus, o, 3) == 8);
        o.field = 4;
        CHECK(invoke(&Obj::field, o) == 4);
        CHECK(invoke([](int a, int b) { return a - b; }, 5, 3) == 2);
        /* invoke_r converts the result, which is how a callable
         * returning int satisfies a signature returning long. */
        CHECK(invoke_r<long>(twice, 2) == 4L);
        invoke_r<void>(twice, 2);        /* discarding is allowed */
    }

    /* ---- mem_fn: a member pointer as a callable ---------------------- */
    {
        Obj o{5, 0};
        auto g = mem_fn(&Obj::get);
        CHECK(g(o) == 5);
        CHECK(g(&o) == 5);               /* through a pointer too */
        auto p = mem_fn(&Obj::plus);
        CHECK(p(o, 3) == 8);
        auto sp = make_shared<Obj>(Obj{7, 0});
        CHECK(g(sp) == 7);               /* and a smart pointer */

        /* Which is the whole point: an algorithm takes a callable and
         * cannot take `&Obj::get`. */
        vector<Obj> v{{1, 0}, {2, 0}, {3, 0}};
        int sum = 0;
        for (const auto &x : v)
            sum += mem_fn(&Obj::get)(x);
        CHECK(sum == 6);
    }

    /* ---- bind and its placeholders ------------------------------------ */
    {
        using namespace placeholders;
        auto add5 = bind(add, 5, _1);
        CHECK(add5(37) == 42);
        /* The placeholders REORDER, which is the one thing a lambda
         * cannot express more briefly. */
        auto swapped = bind(add, _2, _1);
        CHECK(swapped(1, 41) == 42);
        auto sub = [](int a, int b) { return a - b; };
        CHECK(bind(sub, _1, _2)(10, 3) == 7);
        CHECK(bind(sub, _2, _1)(10, 3) == -7);
        /* A bound argument is COPIED at bind time, so a later change to
         * the original does not show. */
        int base = 5;
        auto fixed = bind(add, base, _1);
        base = 100;
        CHECK(fixed(37) == 42);
        /* ref() is how you say you meant otherwise. */
        int live = 5;
        auto through = bind(add, ref(live), _1);
        live = 0;
        CHECK(through(42) == 42);
        /* A member function, with the object bound. */
        Obj o{5, 0};
        auto bp = bind(&Obj::plus, o, _1);
        CHECK(bp(3) == 8);
        /* An unused extra argument is ignored, as the standard says. */
        CHECK(add5(37, 99) == 42);
        static_assert(is_bind_expression_v<decltype(add5)>);
        static_assert(is_placeholder_v<decltype(_2)> == 2);
        static_assert(is_placeholder_v<int> == 0);
    }

    /* ---- error_code: the number AND its namespace --------------------- */
    {
        error_code ok;
        CHECK(!ok);                      /* zero is success everywhere */
        CHECK(ok.value() == 0);

        error_code e = make_error_code(errc::no_such_file_or_directory);
        CHECK((bool)e);
        CHECK(e.value() == ENOENT);
        CHECK(e.category() == generic_category());
        CHECK(string(e.category().name()) == "generic");
        CHECK(e.message() == string(strerror(ENOENT)));

        /* A portable test against a CONDITION, which is the form that
         * survives a platform whose numbers differ. */
        CHECK(e == errc::no_such_file_or_directory);
        CHECK(e != errc::permission_denied);

        /* The same number in another category is another error. The
         * category is compared by ADDRESS, so two categories with the
         * same name would still be two. */
        error_code sys(ENOENT, system_category());
        CHECK(sys.value() == e.value());
        CHECK(sys.category() != e.category());
        CHECK(sys != e);
        /* ... but this platform's numbers ARE the generic ones, so the
         * system code still answers yes to the portable question. */
        CHECK(sys == errc::no_such_file_or_directory);
        CHECK(sys.default_error_condition() ==
              make_error_condition(errc::no_such_file_or_directory));

        error_code z = e;
        z.clear();
        CHECK(!z);
        z.assign(EINVAL, generic_category());
        CHECK(z == errc::invalid_argument);
    }

    /* ---- system_error carries the code, not just a message ------------ */
    {
        int caught = 0;
        try {
            throw system_error(make_error_code(errc::permission_denied),
                               "opening the door");
        } catch (const system_error &ex) {
            caught = ex.code() == errc::permission_denied;
            CHECK(string(ex.what()).find("opening the door") != string::npos);
            CHECK(string(ex.what()).find(ex.code().message()) != string::npos);
        }
        CHECK(caught);
        /* It is a runtime_error, so a handler that knows only the
         * standard hierarchy still catches it. */
        int as_runtime = 0;
        try { throw system_error(make_error_code(errc::io_error)); }
        catch (const runtime_error &) { as_runtime = 1; }
        CHECK(as_runtime);
    }

    /* ---- exception_ptr: an exception that outlives its handler --------- */
    {
        struct Boom { int v; };
        exception_ptr p;
        CHECK(!p);
        /* Outside a handler there is nothing to capture, and that is an
         * answer rather than an error. */
        CHECK(!current_exception());

        try { throw Boom{7}; } catch (...) { p = current_exception(); }
        CHECK((bool)p);

        /* Rethrown with its type INTACT -- a hand-rolled "store the
         * message" loses exactly this. */
        int got = 0;
        try { rethrow_exception(p); }
        catch (const Boom &b) { got = b.v; }
        catch (...) { got = -1; }
        CHECK(got == 7);

        /* And again: it is a counted reference, not a one-shot. The
         * handler that caught it is long gone. */
        got = 0;
        try { rethrow_exception(p); } catch (const Boom &b) { got = b.v; }
        CHECK(got == 7);

        /* Copying is a count, and both refer to the same object. */
        exception_ptr q = p;
        CHECK(q == p);
        exception_ptr r = static_cast<exception_ptr &&>(q);
        CHECK(r == p && !q);
        r = nullptr;
        CHECK(!r);
        got = 0;
        try { rethrow_exception(p); } catch (const Boom &b) { got = b.v; }
        CHECK(got == 7);
    }

    /* make_exception_ptr captures without the caller throwing. */
    {
        auto p = make_exception_ptr(runtime_error("made"));
        int ok = 0;
        try { rethrow_exception(p); }
        catch (const runtime_error &e) { ok = string(e.what()) == "made"; }
        CHECK(ok);
        /* A derived type stays derived. */
        auto q = make_exception_ptr(out_of_range("range"));
        int as_derived = 0, as_base = 0;
        try { rethrow_exception(q); }
        catch (const out_of_range &) { as_derived = 1; }
        catch (const logic_error &) { as_base = 1; }
        CHECK(as_derived && !as_base);
    }

    /* ---- nested_exception: the failure AND its cause ------------------- */
    {
        int outer = 0, inner = 0;
        try {
            try {
                throw runtime_error("the cause");
            } catch (...) {
                throw_with_nested(logic_error("the report"));
            }
        } catch (const logic_error &e) {
            /* The thrown object is BOTH the caller's type and a
             * nested_exception, so an ordinary handler still catches
             * it. */
            outer = string(e.what()) == "the report";
            try { rethrow_if_nested(e); }
            catch (const runtime_error &c) { inner = string(c.what()) == "the cause"; }
        }
        CHECK(outer && inner);

        /* Something that is NOT nested: rethrow_if_nested does nothing
         * rather than throwing something of its own. */
        int disturbed = 0;
        try {
            runtime_error plain("plain");
            rethrow_if_nested(plain);
        } catch (...) {
            disturbed = 1;
        }
        CHECK(!disturbed);
    }

    /* ---- uncaught_exceptions, which is how a scope guard tells a normal
     * exit from an unwinding one --------------------------------------- */
    {
        CHECK(uncaught_exceptions() == 0);
        struct Probe {
            int *out;
            ~Probe() { *out = uncaught_exceptions(); }
        };
        int during = -1;
        try {
            Probe p{&during};
            throw runtime_error("unwinding");
        } catch (...) {
        }
        CHECK(during == 1);
        CHECK(uncaught_exceptions() == 0);
    }

    DONE();
}
