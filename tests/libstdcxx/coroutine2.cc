// More coroutines through libstdc++: function and class template coroutines,
// a generic lambda, a promise built from the parameters with its own
// operator new/delete, get_return_object_on_allocation_failure, co_await in
// ?:, && and ||, nested co_await, structured bindings and a range-for over
// an awaited value, a switch on one, temporaries alive across a suspension,
// and co_yield of a braced list.
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

template <class T> struct Gen {
    struct promise_type {
        T current{};
        Gen get_return_object() { return Gen{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(T v) { current = std::move(v); return {}; }
        void return_void() {}
        void unhandled_exception() { std::abort(); }
    };
    std::coroutine_handle<promise_type> h;
    explicit Gen(std::coroutine_handle<promise_type> p) : h(p) {}
    Gen(Gen &&o) noexcept : h(std::exchange(o.h, {})) {}
    ~Gen() { if (h) h.destroy(); }
    bool next() { h.resume(); return !h.done(); }
    T &value() { return h.promise().current; }
};

// an eager, synchronous task: the value is ready when the ramp returns
template <class T> struct Now {
    struct promise_type {
        std::optional<T> value;
        Now get_return_object() { return Now{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_value(T v) { value.emplace(std::move(v)); }
        void unhandled_exception() { std::abort(); }
    };
    std::coroutine_handle<promise_type> h;
    explicit Now(std::coroutine_handle<promise_type> p) : h(p) {}
    Now(Now &&o) noexcept : h(std::exchange(o.h, {})) {}
    ~Now() { if (h) h.destroy(); }
    bool await_ready() { return true; }
    void await_suspend(std::coroutine_handle<>) {}
    T await_resume() { return std::move(*h.promise().value); }
    T get() { return *h.promise().value; }
};

struct Track {
    std::string s;
    Track(std::string x) : s(std::move(x)) { std::printf("  +%s\n", s.c_str()); }
    Track(const Track &o) : s(o.s + "'") { std::printf("  +%s\n", s.c_str()); }
    ~Track() { std::printf("  -%s\n", s.c_str()); }
};

// a function template coroutine
template <class T> Gen<T> repeat(T v, int n)
{
    for (int i = 0; i < n; i++)
        co_yield v;
}

// a class template's member coroutine
template <class T> struct Box {
    T v;
    Now<T> twice() { co_return v + v; }
};

// a promise constructed from the parameters, with its own allocator
static int allocs;
struct Counted {
    struct promise_type {
        int start;
        promise_type(int s, const char *) : start(s) {}
        static void *operator new(std::size_t n) { allocs++; return std::malloc(n); }
        static void operator delete(void *p) { allocs--; std::free(p); }
        Counted get_return_object() { return Counted{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::abort(); }
    };
    std::coroutine_handle<promise_type> h;
};
Counted counted(int s, const char *name)
{
    std::printf("counted %s from %d\n", name, s);
    co_return;
}

// the frame from the nothrow operator new; none: the promise's return
// object for that
static bool fail_next;
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    return fail_next ? nullptr : std::malloc(n);
}
struct Maybe {
    int v;
    struct promise_type {
        int *out;
        promise_type(int *o) : out(o) {}
        static Maybe get_return_object_on_allocation_failure() { return Maybe{-1}; }
        Maybe get_return_object() { return Maybe{1}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_value(int x) { *out = x; }
        void unhandled_exception() {}
    };
};
Maybe make(int *out) { co_return 42; }

Now<int> value_of(int x) { co_return x; }
Now<Now<int>> nested() { co_return value_of(7); }
Now<std::pair<int, std::string>> pairing() { co_return {3, "three"}; }

Now<int> conditions(bool flag)
{
    int r = flag ? co_await value_of(10) : co_await value_of(20);
    if (flag && co_await value_of(1) == 1)
        r += 1;
    if (!flag || co_await value_of(0))
        r += 2;
    int twice = co_await co_await nested();
    auto [n, name] = co_await pairing();
    r += twice + n + (int)name.size();
    for (int x : co_await Now<std::vector<int>>([]() -> Now<std::vector<int>> { co_return std::vector<int>{1, 2, 3}; }()))
        r += x * 100;
    switch (co_await value_of(2)) {
    case 1: r = -1; break;
    case 2: r += co_await value_of(1000); break;
    }
    co_return r;
}

Gen<std::string> temps(std::string base)
{
    co_yield base + "-a";
    co_yield Track(base + "-mid").s;     // the Track lives until after the resume
    co_yield base + "-z";
}

Gen<int> braced()
{
    co_yield {};
    co_yield {5};
}

int main()
{
    Gen<char> g = repeat('x', 3);
    int n = 0;
    while (g.next())
        n += g.value() == 'x';
    std::printf("repeat %d\n", n);
    Box<std::string> b{"ab"};
    std::printf("box %s\n", b.twice().get().c_str());
    counted(4, "c");
    std::printf("allocs after %d\n", allocs);
    auto gl = [](auto x) -> Now<decltype(x)> { co_return x * 3; };
    std::printf("generic lambda %d %g\n", gl(5).get(), gl(1.5).get());
    std::printf("conditions %d %d\n", conditions(true).get(), conditions(false).get());
    {
        auto t = temps("t");
        while (t.next()) {
            std::printf("got %s\n", t.value().c_str());
            if (t.value() == "t-mid")
                break;              // destroyed with the Track still alive
        }
        std::printf("leaving\n");
    }
    auto br = braced();
    int bs = 0;
    while (br.next())
        bs += br.value();
    std::printf("braced %d\n", bs);
    int r1 = 0, r2 = 0;
    Maybe m1 = make(&r1);
    fail_next = true;
    Maybe m2 = make(&r2);
    fail_next = false;
    std::printf("allocation %d %d, failed %d %d\n", m1.v, r1, m2.v, r2);
    return n == 3 && allocs == 0 && m2.v == -1 ? 42 : 1;
}
