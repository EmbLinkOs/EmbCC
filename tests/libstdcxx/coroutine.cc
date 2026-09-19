// C++20 coroutines through libstdc++'s <coroutine>: a generator (range-for
// over it, early destruction running the locals' destructors), a lazy task
// resumed by symmetric transfer (await_suspend returning a handle,
// std::noop_coroutine), exceptions kept by the promise and rethrown to the
// awaiter, operator co_await, await_transform with a bool await_suspend,
// member and lambda coroutines, and co_await in conditions and loops.
#include <coroutine>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

// ---- a generator ----
template <class T> struct Generator {
    struct promise_type {
        T current;
        Generator get_return_object() { return Generator{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        template <class U> std::suspend_always yield_value(U &&v) { current = std::forward<U>(v); return {}; }
        void return_void() {}
        void unhandled_exception() { throw; }
    };
    struct iterator {
        std::coroutine_handle<promise_type> h;
        iterator &operator++() { h.resume(); return *this; }
        const T &operator*() const { return h.promise().current; }
        bool operator==(std::default_sentinel_t) const { return h.done(); }
    };
    std::coroutine_handle<promise_type> h;
    explicit Generator(std::coroutine_handle<promise_type> p) : h(p) {}
    Generator(Generator &&o) noexcept : h(std::exchange(o.h, {})) {}
    ~Generator() { if (h) h.destroy(); }
    iterator begin() { h.resume(); return iterator{h}; }
    std::default_sentinel_t end() { return {}; }
};

struct Noisy {
    const char *name;
    explicit Noisy(const char *n) : name(n) { std::printf("make %s\n", name); }
    ~Noisy() { std::printf("drop %s\n", name); }
};

Generator<int> range(int from, int to)
{
    Noisy n("range-local");
    for (int i = from; i < to; i++)
        co_yield i;
}

Generator<std::string> words(std::string prefix, int n)
{
    for (int i = 0; i < n; i++)
        co_yield prefix + std::to_string(i);
}

// ---- a lazy task, symmetric transfer ----
template <class T> struct Task {
    struct promise_type {
        T value{};
        std::exception_ptr error;
        std::coroutine_handle<> cont;
        Task get_return_object() { return Task{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct Final {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                auto c = h.promise().cont;
                return c ? c : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        Final final_suspend() noexcept { return {}; }
        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { error = std::current_exception(); }
    };
    std::coroutine_handle<promise_type> h;
    explicit Task(std::coroutine_handle<promise_type> p) : h(p) {}
    Task(Task &&o) noexcept : h(std::exchange(o.h, {})) {}
    ~Task() { if (h) h.destroy(); }
    struct Awaiter {
        std::coroutine_handle<promise_type> h;
        bool await_ready() { return false; }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> c) { h.promise().cont = c; return h; }
        T await_resume() {
            if (h.promise().error)
                std::rethrow_exception(h.promise().error);
            return std::move(h.promise().value);
        }
    };
    Awaiter operator co_await() && { return Awaiter{h}; }
    T run() {
        h.resume();
        if (h.promise().error)
            std::rethrow_exception(h.promise().error);
        return h.promise().value;
    }
};

Task<int> leaf(int x) { co_return x * 2; }
Task<int> fails(int x)
{
    if (x > 0)
        throw std::string("boom");
    co_return 0;
}
Task<int> sum3()
{
    int a = co_await leaf(1);
    int b = 10 + co_await leaf(2) * 3;
    int c = 0;
    if (co_await leaf(3) == 6)
        c = 100;
    co_return a + b + c;
}
Task<std::string> catcher()
{
    try {
        co_await fails(1);
    } catch (const std::string &s) {
        co_return "caught " + s;
    }
    co_return "none";
}
Task<int> loops()
{
    int n = 0, i = 0;
    while (co_await leaf(i) < 10)
        n += ++i;
    for (int k = 0; k < 3; k++)
        n += co_await leaf(k);
    co_return n;
}

// ---- await_transform, bool await_suspend, a member coroutine ----
struct Counter {
    int base = 5;
    struct Ready {
        int v;
        bool await_ready() { return false; }
        bool await_suspend(std::coroutine_handle<>) { return false; }  // do not suspend after all
        int await_resume() { return v; }
    };
    struct Tx {
        struct promise_type {
            int out = 0;
            Tx get_return_object() { return Tx{std::coroutine_handle<promise_type>::from_promise(*this)}; }
            std::suspend_never initial_suspend() noexcept { return {}; }
            std::suspend_always final_suspend() noexcept { return {}; }
            Ready await_transform(int v) { return Ready{v * 3}; }
            void return_value(int v) { out = v; }
            void unhandled_exception() {}
        };
        std::coroutine_handle<promise_type> h;
        ~Tx() { h.destroy(); }
    };
    Tx compute(int x) { co_return base + co_await x; }
};

// ---- early destruction ----
Generator<int> guarded()
{
    Noisy a("outer");
    for (int i = 0;; i++) {
        Noisy b("inner");
        co_yield i;
    }
}

int main()
{
    int s = 0;
    for (int v : range(1, 5))
        s += v;
    std::printf("range sum %d\n", s);
    for (auto &w : words("w", 3))
        std::printf("%s\n", w.c_str());
    std::printf("sum3 %d\n", sum3().run());
    std::printf("%s\n", catcher().run().c_str());
    std::printf("loops %d\n", loops().run());
    Counter c;
    auto tx = c.compute(4);
    std::printf("member %d done %d\n", tx.h.promise().out, (int)tx.h.done());
    {
        auto g = guarded();
        auto it = g.begin();
        ++it;
        std::printf("at %d, dropping\n", *it);
    }
    auto lam = [](int k) -> Generator<int> { for (int i = 0; i < k; i++) co_yield i * i; };
    int sq = 0;
    for (int v : lam(4))
        sq += v;
    std::printf("lambda %d\n", sq);
    try {
        fails(1).run();
    } catch (const std::string &e) {
        std::printf("rethrown %s\n", e.c_str());
    }
    return s == 10 && sq == 14 ? 42 : 1;
}
