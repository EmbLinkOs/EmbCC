// CX7: coroutines — co_await, co_yield and co_return; the promise from
// std::coroutine_traits (its primary, and a specialization making a void
// function a coroutine); std::coroutine_handle over the __builtin_coro_*
// builtins, laid out as libstdc++'s; a frame holding the parameters'
// copies, the locals, the awaiters and temporaries alive across a
// suspension; await_suspend returning void, bool or another coroutine's
// handle (symmetric transfer); operator co_await (member and not) and
// await_transform; a promise made from the parameters, with its own
// operator new and delete; initial and final suspends that do and do
// not suspend; co_await in conditions, loops, a switch, && and ?:;
// member, lambda and template coroutines; exceptions caught by the
// promise or passed to the resumer; and a coroutine destroyed while
// suspended destroying what is alive there.
// expect-exit: 42
#include <stdio.h>
#include <stdlib.h>

namespace std {
typedef decltype(sizeof 0) size_t;
template <class R, class... A> struct coroutine_traits {
    using promise_type = typename R::promise_type;
};
template <class P = void> struct coroutine_handle;
template <> struct coroutine_handle<void> {
    void *fr = nullptr;
    static coroutine_handle from_address(void *a) { coroutine_handle h; h.fr = a; return h; }
    void *address() const { return fr; }
    bool done() const { return __builtin_coro_done(fr); }
    void resume() const { __builtin_coro_resume(fr); }
    void destroy() const { __builtin_coro_destroy(fr); }
    explicit operator bool() const { return fr != nullptr; }
};
template <class P> struct coroutine_handle {
    void *fr = nullptr;
    static coroutine_handle from_address(void *a) { coroutine_handle h; h.fr = a; return h; }
    static coroutine_handle from_promise(P &p)
    {
        coroutine_handle h;
        h.fr = __builtin_coro_promise((char *)&p, __alignof(P), true);
        return h;
    }
    void *address() const { return fr; }
    operator coroutine_handle<>() const { return coroutine_handle<>::from_address(fr); }
    bool done() const { return __builtin_coro_done(fr); }
    void resume() const { __builtin_coro_resume(fr); }
    void destroy() const { __builtin_coro_destroy(fr); }
    P &promise() const { return *(P *)__builtin_coro_promise(fr, __alignof(P), false); }
    explicit operator bool() const { return fr != nullptr; }
};
struct suspend_always {
    bool await_ready() const noexcept { return false; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};
struct suspend_never {
    bool await_ready() const noexcept { return true; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};
}

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static int alive;                 // Noisy objects not yet destroyed
struct Noisy {
    int v;
    Noisy(int x) : v(x) { alive++; }
    Noisy(const Noisy &o) : v(o.v) { alive++; }
    ~Noisy() { alive--; }
};

// ---- a generator ----
template <class T> struct Gen {
    struct promise_type {
        T cur{};
        Gen get_return_object() { return Gen{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(T v) { cur = v; return {}; }
        void return_void() {}
        void unhandled_exception() { throw; }
    };
    std::coroutine_handle<promise_type> h;
    Gen(std::coroutine_handle<promise_type> p) : h(p) {}
    Gen(Gen &&o) : h(o.h) { o.h = {}; }
    ~Gen() { if (h) h.destroy(); }
    bool next() { h.resume(); return !h.done(); }
    T value() const { return h.promise().cur; }
};

Gen<int> squares(int n)
{
    Noisy guard(n);                        // a local alive across yields
    for (int i = 1; i <= n; i++)
        co_yield i * i;
}

Gen<int> forever(Noisy by_value)       // a parameter's copy in the frame
{
    Noisy outer(1);
    for (int i = 0;; i++) {
        Noisy inner(i);
        co_yield by_value.v + inner.v;
    }
}

template <class T> Gen<T> repeat(T v, int n)
{
    while (n-- > 0)
        co_yield v;
}

// ---- a lazy task, resumed by whoever awaits it ----
template <class T> struct Task {
    struct promise_type {
        T value{};
        std::coroutine_handle<> cont;
        bool failed = false;
        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() { return {}; }
        struct Final {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept
            {
                return h.promise().cont;           // symmetric transfer
            }
            void await_resume() noexcept {}
        };
        Final final_suspend() noexcept { return {}; }
        void return_value(T v) { value = v; }
        void unhandled_exception() { failed = true; }
    };
    std::coroutine_handle<promise_type> h;
    Task(std::coroutine_handle<promise_type> p) : h(p) {}
    Task(Task &&o) : h(o.h) { o.h = {}; }
    ~Task() { if (h) h.destroy(); }
};

template <class T> struct TaskAwaiter {
    std::coroutine_handle<typename Task<T>::promise_type> h;
    bool await_ready() { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> c)
    {
        h.promise().cont = c;
        return h;
    }
    T await_resume() { return h.promise().failed ? -1 : h.promise().value; }
};
template <class T> TaskAwaiter<T> operator co_await(Task<T> &&t) { return {t.h}; }

// a coroutine that runs a task to completion from outside one
struct Root {
    struct promise_type {
        Root get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};
template <class T> Root drive(Task<T> t, T *out) { *out = co_await static_cast<Task<T> &&>(t); }
template <class T> T run(Task<T> t)
{
    T r{};
    drive(static_cast<Task<T> &&>(t), &r);
    return r;
}

Task<int> leaf(int x) { co_return x * 2; }
Task<int> thrower(int x)
{
    if (x)
        throw x;
    co_return 1;
}
Task<int> chain()
{
    int a = co_await leaf(1);                          // 2
    int b = 10 + co_await leaf(2) * 3;                 // 22
    int c = co_await thrower(1);                       // the promise caught it: -1
    co_return a + b + c;                               // 23
}
Task<int> control(bool f)
{
    int r = f ? co_await leaf(5) : co_await leaf(50);  // 10 or 100
    if (f && co_await leaf(1) == 2)
        r += 1;
    if (!f || co_await leaf(0))
        r += 2;
    int i = 0;
    while (co_await leaf(i) < 6)                        // i to 3
        i++;
    do
        r += 1;
    while (co_await leaf(i--) > 2);                      // 3 times
    for (int k = 0; k < co_await leaf(2); k += co_await leaf(1) / 2)
        r += 100;                                        // 4 times
    switch (co_await leaf(3)) {
    case 6: r += 1000; break;
    default: r = -1;
    }
    co_return r + i;
}

// ---- await_suspend returning bool, await_transform, a member coroutine ----
struct Calc {
    int base = 30;
    struct Now {
        int v;
        bool await_ready() { return false; }
        bool await_suspend(std::coroutine_handle<>) { return false; }   // on we go
        int await_resume() { return v; }
    };
    struct Result {
        struct promise_type {
            int out = 0;
            Result get_return_object() { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
            std::suspend_never initial_suspend() { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }
            Now await_transform(int v) { return Now{v + 1}; }
            void return_value(int v) { out = v; }
            void unhandled_exception() {}
        };
        std::coroutine_handle<promise_type> h;
    };
    Result compute(int x) { co_return base + co_await x; }
};

// ---- the promise from the parameters, its own allocation ----
static int frames, promise_arg;
struct Counted {
    struct promise_type {
        promise_type(int a, int) { promise_arg = a; }
        static void *operator new(std::size_t n) { frames++; return malloc(n); }
        static void operator delete(void *p) { frames--; free(p); }
        Counted get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};
Counted counted(int a, int b)
{
    promise_arg += b;
    co_return;
}

// ---- a void function made a coroutine by a specialization ----
static int fired;
struct Fire {
    Fire get_return_object() { return {}; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
};
template <> struct std::coroutine_traits<void, int> { using promise_type = Fire; };
void fire(int n)
{
    co_await std::suspend_never{};
    fired = n;
}

int main()
{
    {
        Gen<int> g = squares(4);
        int s = 0;
        while (g.next())
            s += g.value();
        check("a generator", s == 30);
    }
    check("its locals destroyed at the end", alive == 0);
    {
        Gen<int> f = forever(Noisy(40));
        f.next();
        f.next();
        check("suspended with locals alive", f.value() == 41 && alive == 3);
    }
    check("destroyed while suspended", alive == 0);
    {
        Gen<char> r = repeat('x', 3);
        int n = 0;
        while (r.next())
            n += r.value() == 'x';
        check("a function template coroutine", n == 3);
    }
    check("co_await, symmetric transfer", run(chain()) == 23);
    check("co_await in conditions and loops", run(control(true)) == 1414 &&
                                              run(control(false)) == 1505);
    Calc c;
    auto res = c.compute(11);
    check("bool await_suspend, await_transform",
          res.h.done() && res.h.promise().out == 42);
    res.h.destroy();
    counted(3, 4);
    check("the promise from the parameters, its operator new",
          promise_arg == 7 && frames == 0);
    auto lam = [](int k) -> Gen<int> { for (int i = 0; i < k; i++) co_yield i; };
    Gen<int> lg = lam(5);
    int ls = 0;
    while (lg.next())
        ls += lg.value();
    check("a lambda coroutine", ls == 10);
    fire(9);
    check("a void coroutine through coroutine_traits", fired == 9);
    auto boom = []() -> Gen<int> { co_yield 1; throw 7; };
    Gen<int> bg = boom();
    int caught = 0;
    bg.next();
    try {
        bg.next();
    } catch (int e) {
        caught = e;
    }
    check("an exception to the resumer", caught == 7 && bg.h.done());

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
