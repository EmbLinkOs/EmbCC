// Templates, constexpr and namespaces: compile-time configuration the way
// an RTOS wrapper uses it -- sizes, register maps and policies decided by
// the compiler, not at run time. The data model is checked here too, by
// relations that hold on every target, so the host build agrees. Each
// line printed is one property; the host's clang++ build prints the same.
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

// ---- the data model, as relations ----
static_assert(sizeof(size_t) == sizeof(void *), "size_t is pointer-sized");
static_assert(sizeof(ptrdiff_t) == sizeof(void *), "ptrdiff_t too");
static_assert(sizeof(intptr_t) == sizeof(void *), "intptr_t too");
static_assert(sizeof(long) >= sizeof(int), "long");
static_assert(sizeof(long long) == 8 && alignof(long long) == 8, "ll");
static_assert(sizeof(double) == 8 && alignof(double) == 8, "double");
static_assert(sizeof(int &) == sizeof(int), "a reference is its object");
static_assert(sizeof(decltype(sizeof 0)) == sizeof(size_t), "sizeof's type");

struct WithRef { char c; int &r; };
static_assert(sizeof(WithRef) == 2 * sizeof(void *), "a reference member");
struct Poly { virtual ~Poly() {} char c; };
static_assert(sizeof(Poly) == 2 * sizeof(void *), "vptr plus a char");
static_assert(alignof(Poly) == alignof(void *), "aligned as the vptr");
struct M { void f(); virtual void g(); };
static_assert(sizeof(&M::f) == 2 * sizeof(void *), "PMF: two words");
static_assert(sizeof(int M::*) == sizeof(ptrdiff_t), "pointer to data");
struct LL { char c; long long x; };
static_assert(sizeof(LL) == 16 && offsetof(LL, x) == 8, "long long aligned");

// ---- templates ----
template <typename T> constexpr T max_of(T a, T b) { return a > b ? a : b; }

template <unsigned N> struct Fact { static constexpr unsigned long long value = N * Fact<N - 1>::value; };
template <> struct Fact<0> { static constexpr unsigned long long value = 1; };

template <typename T, size_t N>
constexpr size_t count_of(const T (&)[N]) { return N; }

template <typename T> struct Traits { static constexpr const char *name = "other"; };
template <> struct Traits<int> { static constexpr const char *name = "int"; };
template <typename T> struct Traits<T *> { static constexpr const char *name = "pointer"; };

template <typename... Ts> constexpr int count_args(Ts...) { return sizeof...(Ts); }
template <typename... Ts> constexpr long sum(Ts... xs) { return (0L + ... + xs); }

namespace hw {
// A register map: base addresses and bit fields, all compile-time.
template <uintptr_t Base> struct Port {
    static constexpr uintptr_t base = Base;
    static constexpr uintptr_t reg(unsigned off) { return Base + off; }
};
constexpr uint32_t bit(unsigned n) { return 1u << n; }
constexpr uint32_t mask(unsigned lo, unsigned hi)
{
    uint32_t m = 0;
    for (unsigned i = lo; i <= hi; i++)
        m |= bit(i);
    return m;
}
using GpioA = Port<0x40020000>;
}  // namespace hw

// A policy-based ring buffer.
struct Overwrite { static constexpr bool drop_new = false; };
struct Drop { static constexpr bool drop_new = true; };

template <typename T, unsigned N, typename Policy = Drop>
class Ring {
    static_assert((N & (N - 1)) == 0, "a power of two");
    T data_[N];
    unsigned head_ = 0, tail_ = 0;
public:
    static constexpr unsigned capacity = N;
    bool put(T v) {
        if (tail_ - head_ == N) {
            if (Policy::drop_new)
                return false;
            head_++;
        }
        data_[tail_++ & (N - 1)] = v;
        return true;
    }
    bool get(T &v) {
        if (head_ == tail_)
            return false;
        v = data_[head_++ & (N - 1)];
        return true;
    }
};

template <typename T> struct Box {
    T v;
    template <typename U> Box<U> as() const { return Box<U>{static_cast<U>(v)}; }
};

constexpr int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
constexpr int table[] = { fib(5), fib(10), fib(15) };
static_assert(table[2] == 610, "constexpr evaluation");

enum class Prio : uint8_t { Low = 1, Normal = 4, High = 7 };
static_assert(sizeof(Prio) == 1, "an enum's fixed underlying type");

int main()
{
    printf("max %d %ld %.2f\n", max_of(3, 9), max_of(-4L, -2L),
           max_of(1.5, 0.25));
    printf("fact %llu %llu\n", Fact<10>::value, Fact<20>::value);
    int arr[7];
    printf("count %d args %d sum %ld\n", (int)count_of(arr),
           count_args(1, 'c', 2.0, "s"), sum(1, 2L, 3, 40));
    printf("traits %s %s %s\n", Traits<int>::name, Traits<char *>::name,
           Traits<double>::name);
    constexpr uintptr_t moder = hw::GpioA::reg(0x14);
    printf("reg %lx mask %lx\n", (unsigned long)moder,
           (unsigned long)hw::mask(4, 7));

    Ring<int, 4> drop;
    Ring<int, 4, Overwrite> over;
    for (int i = 1; i <= 6; i++) {
        bool a = drop.put(i), b = over.put(i);
        printf("put %d %d %d\n", i, a, b);
    }
    int v;
    printf("drop:");
    while (drop.get(v))
        printf(" %d", v);
    printf("\nover:");
    while (over.get(v))
        printf(" %d", v);
    printf("\ncapacity %u\n", Ring<char, 8>::capacity);

    Box<double> bd{3.75};
    Box<int> bi = bd.as<int>();
    printf("box %d table %d %d %d\n", bi.v, table[0], table[1], table[2]);
    Prio p = Prio::High;
    printf("prio %d\n", static_cast<int>(p));
    long long big = 1LL << 40;
    unsigned long long ub = ~0ULL;
    printf("wide %lld %llu %d\n", big + 1, ub, (int)(big >> 38));
    printf("==END==\n");
    fflush(stdout);
    return 0;
}
