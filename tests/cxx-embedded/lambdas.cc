// Lambdas without captures (as C callbacks: an ISR table, a thread entry)
// and with captures (by copy, by reference, this, init-captures), and
// templates taking callables. Each line printed is one property; the
// host's clang++ build prints the same.
#include <stdio.h>

typedef void (*isr_t)(void);
typedef int (*entry_t)(void *arg);

static int hits[4];
static isr_t vectors[4];

// A thread-creation API as a C kernel has it: a function pointer and an
// argument.
static int start_thread(entry_t entry, void *arg) { return entry(arg); }

template <typename F> int twice(F f, int x) { return f(f(x)); }

template <typename F> struct Deferred {
    F f;
    ~Deferred() { f(); }
};
template <typename F> Deferred<F> defer(F f) { return Deferred<F>{f}; }

class Timer {
public:
    explicit Timer(int period) : period_(period) {}
    template <typename F> int each(int n, F f) {
        int total = 0;
        for (int i = 0; i < n; i++)
            total += f(i * period_);
        return total;
    }
    int scaled(int k) {
        auto g = [this, k](int t) { return t * k + period_; };
        return each(3, g);
    }
private:
    int period_;
};

struct Big { long a, b, c, d; };

int main()
{
    // captureless: converted to function pointers
    vectors[0] = [] { hits[0]++; };
    vectors[1] = [] { hits[1] += 10; };
    vectors[2] = +[] { hits[2] = 7; };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i <= k; i++)
            vectors[i]();
    printf("isr hits %d %d %d\n", hits[0], hits[1], hits[2]);

    int shared = 5;
    int r = start_thread([](void *p) { return *static_cast<int *>(p) * 3; },
                         &shared);
    printf("thread %d\n", r);

    // captures
    int base = 100, count = 0;
    auto by_copy = [base](int x) { return base + x; };
    auto by_ref = [&count](int x) { count += x; return count; };
    base = 200;                            // the copy keeps 100
    int c1 = by_copy(1), r1 = by_ref(2), r2 = by_ref(3);
    printf("copy %d ref %d %d count %d\n", c1, r1, r2, count);
    auto mixed = [=, &count](int x) mutable { base++; count++; return base + x; };
    int m1 = mixed(1), m2 = mixed(1);
    printf("mutable %d %d base %d count %d\n", m1, m2, base, count);
    auto init = [n = base * 2, s = 0]() mutable { return n + ++s; };
    int i1 = init(), i2 = init();
    printf("init-capture %d %d\n", i1, i2);
    Big big = { 1, 2, 3, 4 };
    auto sum = [big] { return big.a + big.b + big.c + big.d; };
    big.a = 1000;
    printf("big %ld\n", sum());

    printf("twice %d %d\n", twice([](int x) { return x * 3; }, 2),
           twice([&](int x) { return x + base; }, 1));
    Timer t(10);
    printf("timer %d %d\n", t.each(4, [](int v) { return v; }), t.scaled(2));
    {
        auto d = defer([&] { printf("deferred count %d\n", count); });
        count = 42;
    }
    auto gen = [](auto a, auto b) { return a + b; };
    printf("generic %d %.1f\n", gen(2, 3), gen(1.5, 2.0));
    auto fact = [](int n) {
        int f = 1;
        for (int i = 2; i <= n; i++)
            f *= i;
        return f;
    };
    constexpr auto sq = [](int x) { return x * x; };
    static_assert(sq(7) == 49, "a constexpr lambda");
    printf("fact %d sq %d\n", fact(6), sq(9));
    printf("==END==\n");
    fflush(stdout);
    return 0;
}
