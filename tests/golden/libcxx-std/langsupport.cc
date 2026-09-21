/* <coroutine>, <source_location>, <bit>, <numbers>, <version>.
 *
 * The four headers that exist because the LANGUAGE needs them, plus the
 * one that exists so a program can ask what the library has.
 *
 * `<coroutine>` and `<source_location>` are not conveniences: without
 * the first, `co_await` does not compile at all, because the compiler
 * looks up `std::coroutine_traits` to find the promise type and
 * `std::coroutine_handle` to hand one back. Without the second, there
 * is no way to write a logging FUNCTION that reports its caller --
 * only a macro.
 *
 * `<bit>` is the operations every machine has had for decades and C++
 * had no way to ask for, and `bit_cast` is the only DEFINED way to
 * reinterpret an object's bytes.
 */
#include "check.h"
#include <bit>
#include <coroutine>
#include <cstdint>
#include <numbers>
#include <source_location>
#include <string>
#include <version>

using namespace std;

/* ---- a generator, which is what coroutines are for ------------------- */
struct Gen {
    struct promise_type {
        int value = 0;
        bool done = false;
        Gen get_return_object()
        { return Gen{coroutine_handle<promise_type>::from_promise(*this)}; }
        /* Suspend at the start, so the body does not run until the
         * caller asks: a generator that ran to its first yield on
         * construction would be surprising and would make an empty
         * generator do work. */
        suspend_always initial_suspend() noexcept { return {}; }
        /* Suspend at the END, so the frame is still alive when the
         * caller checks done() and reads the last value. Returning
         * suspend_never here destroys the frame as the coroutine ends,
         * and promise() then reads freed memory. */
        suspend_always final_suspend() noexcept { return {}; }
        suspend_always yield_value(int v) { value = v; return {}; }
        void return_void() { done = true; }
        void unhandled_exception() {}
    };

    coroutine_handle<promise_type> h;
    explicit Gen(coroutine_handle<promise_type> x) : h(x) {}
    Gen(const Gen &) = delete;
    Gen(Gen &&o) noexcept : h(o.h) { o.h = {}; }
    ~Gen() { if (h) h.destroy(); }

    bool next() { h.resume(); return !h.done(); }
    int value() const { return h.promise().value; }
};

static Gen count_to(int n)
{
    for (int i = 1; i <= n; i++)
        co_yield i;
}

static Gen empty_gen() { co_return; }

/* A coroutine that awaits a value, to exercise the other keyword. */
struct Awaiter {
    int v;
    bool await_ready() const noexcept { return true; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    int await_resume() const noexcept { return v; }
};

struct Task {
    struct promise_type {
        int result = 0;
        Task get_return_object()
        { return Task{coroutine_handle<promise_type>::from_promise(*this)}; }
        suspend_never initial_suspend() noexcept { return {}; }
        suspend_always final_suspend() noexcept { return {}; }
        void return_value(int v) { result = v; }
        void unhandled_exception() {}
    };
    coroutine_handle<promise_type> h;
    explicit Task(coroutine_handle<promise_type> x) : h(x) {}
    Task(const Task &) = delete;
    ~Task() { if (h) h.destroy(); }
    int result() const { return h.promise().result; }
};

static Task add_awaited()
{
    int a = co_await Awaiter{40};
    int b = co_await Awaiter{2};
    co_return a + b;
}

/* ---- source_location as a default argument ---------------------------- */
static string seen_file;
static unsigned seen_line;
static string seen_fn;

static void note(source_location l = source_location::current())
{
    seen_file = l.file_name();
    seen_line = (unsigned)l.line();
    seen_fn = l.function_name();
}

int main()
{
    /* ---- coroutines ---------------------------------------------------- */
    {
        Gen g = count_to(4);
        int sum = 0, n = 0;
        while (g.next()) {
            sum += g.value();
            n++;
        }
        CHECK(n == 4 && sum == 10);
        /* It ran to the end and is suspended at the final suspend --
         * which is what makes reading the promise after the fact legal
         * rather than a use-after-free. */
        CHECK(g.h.done());

        Gen e = empty_gen();
        CHECK(!e.next());
        CHECK(e.h.done());

        /* co_await and co_return. */
        Task t = add_awaited();
        CHECK(t.result() == 42);
        CHECK(t.h.done());
    }

    /* A handle is ONE POINTER: copying copies the address, and the
     * type-erased form is the same pointer. */
    {
        Gen g = count_to(2);
        coroutine_handle<Gen::promise_type> a = g.h;
        coroutine_handle<> erased = a;
        CHECK(a.address() == erased.address());
        CHECK(a == g.h);
        CHECK(a.address() == g.h.address());
        /* from_promise inverts promise(): both are the same fixed
         * offset in the frame, computed by the compiler. */
        auto back = coroutine_handle<Gen::promise_type>::from_promise(
            a.promise());
        CHECK(back.address() == a.address());
        CHECK(coroutine_handle<Gen::promise_type>::from_address(a.address())
              == a);

        /* A null handle is false; a FINISHED one is still true, because
         * the frame is there and still has to be destroyed. Confusing
         * the two leaks. */
        coroutine_handle<> nil;
        CHECK(!nil);
        CHECK(nil.address() == nullptr);
        CHECK((bool)erased);
        while (g.next()) {}
        CHECK((bool)g.h && g.h.done());
    }

    /* noop_coroutine: never done, does nothing, and has an address --
     * the "stop here" a symmetric transfer needs when there is nobody
     * to transfer to. */
    {
        auto n = noop_coroutine();
        CHECK((bool)n);
        CHECK(!n.done());
        CHECK(n.address() != nullptr);
        n.resume();
        n.resume();
        CHECK(!n.done());
        coroutine_handle<> e = n;
        CHECK(e.address() == n.address());
    }

    /* ---- source_location ------------------------------------------------ */
    {
        auto here = source_location::current();
        unsigned line_of_here = (unsigned)here.line();
        CHECK(line_of_here > 0);
        CHECK(string(here.file_name()).find("langsupport") != string::npos);
        CHECK(string(here.function_name()).find("main") != string::npos);
        CHECK(here.column() > 0);

        /* The whole point: as a default argument it is evaluated at the
         * CALL SITE, so a logging function reports its caller rather
         * than itself. A macro is the only other way to get this. */
        note();
        unsigned call_line = __LINE__ - 1;
        CHECK(seen_line == call_line);
        CHECK(seen_fn.find("main") != string::npos);
        CHECK(seen_file == here.file_name());

        /* Two calls on different lines report different lines -- which
         * is the check that it is not simply reporting where `note` is
         * defined. */
        note();
        CHECK(seen_line == (unsigned)(__LINE__ - 1));
        CHECK(seen_line != call_line);

        /* A default-constructed one answers rather than crashing. */
        source_location d;
        CHECK(string(d.file_name()) == "");
        CHECK(d.line() == 0 && d.column() == 0);
    }

    /* ---- bit_cast -------------------------------------------------------- */
    {
        /* The IEEE bit pattern of 1.0f, which is the check that the
         * bytes really are reinterpreted and not converted. */
        CHECK(bit_cast<uint32_t>(1.0f) == 0x3f800000u);
        CHECK(bit_cast<float>(0x3f800000u) == 1.0f);
        CHECK(bit_cast<uint64_t>(1.0) == 0x3ff0000000000000ull);
        CHECK(bit_cast<double>(0x3ff0000000000000ull) == 1.0);
        /* A conversion would give 1; a bit_cast gives the pattern. */
        CHECK(bit_cast<uint32_t>(1.0f) != 1u);
        CHECK(bit_cast<int32_t>(-0.0f) == (int32_t)0x80000000);

        struct Pair { uint16_t a, b; };
        Pair p = bit_cast<Pair>((uint32_t)0x00070005u);
        CHECK(p.a == 5 && p.b == 7);     /* little-endian, as the enum says */
        CHECK(bit_cast<uint32_t>(p) == 0x00070005u);
    }

    /* ---- endian ---------------------------------------------------------- */
    {
        static_assert(endian::native == endian::little);
        static_assert(endian::little != endian::big);
        /* And the claim is checkable against the bytes themselves. */
        CHECK(bit_cast<uint8_t>((uint8_t)1) == 1);
        struct Bytes { uint8_t b[4]; };
        Bytes bs = bit_cast<Bytes>((uint32_t)0x01020304u);
        CHECK(bs.b[0] == 4 && bs.b[3] == 1);
    }

    /* ---- byteswap --------------------------------------------------------- */
    {
        CHECK(byteswap((uint16_t)0x1234) == 0x3412);
        CHECK(byteswap((uint32_t)0x12345678u) == 0x78563412u);
        CHECK(byteswap((uint64_t)0x0123456789abcdefull) == 0xefcdab8967452301ull);
        CHECK(byteswap((uint8_t)0x12) == 0x12);
        CHECK(byteswap(byteswap((uint32_t)0xdeadbeefu)) == 0xdeadbeefu);
    }

    /* ---- rotates: the count-zero case that is undefined if written by
     * hand as `(x << n) | (x >> (w - n))` -------------------------------- */
    {
        CHECK(rotl((uint8_t)0b10110011, 0) == 0b10110011);
        CHECK(rotr((uint8_t)0b10110011, 0) == 0b10110011);
        CHECK(rotl((uint8_t)0b10110011, 1) == 0b01100111);
        CHECK(rotr((uint8_t)0b10110011, 1) == 0b11011001);
        CHECK(rotl((uint32_t)1, 32) == 1u);        /* a full turn */
        CHECK(rotl((uint32_t)1, 31) == 0x80000000u);
        /* A negative count rotates the other way, which the standard
         * requires and a naive implementation shifts by a huge number
         * instead. */
        CHECK(rotl((uint8_t)0b10110011, -1) == rotr((uint8_t)0b10110011, 1));
        CHECK(rotr((uint16_t)0x0001, -1) == 2);
        static_assert(rotl((uint32_t)0x80000000u, 1) == 1u);
    }

    /* ---- counting: every one defined at ZERO, where the raw builtins
     * are not ---------------------------------------------------------- */
    {
        CHECK(countl_zero((uint8_t)0) == 8);
        CHECK(countl_zero((uint32_t)0) == 32);
        CHECK(countl_zero((uint64_t)0) == 64);
        CHECK(countr_zero((uint8_t)0) == 8);
        CHECK(countr_zero((uint32_t)0) == 32);
        CHECK(popcount((uint32_t)0) == 0);

        CHECK(countl_zero((uint8_t)0b00010000) == 3);
        CHECK(countl_zero((uint32_t)1) == 31);
        CHECK(countl_zero((uint16_t)1) == 15);   /* the WIDTH matters */
        CHECK(countr_zero((uint32_t)8) == 3);
        CHECK(countl_one((uint8_t)0b11100000) == 3);
        CHECK(countr_one((uint8_t)0b00000111) == 3);
        CHECK(countl_one((uint8_t)0xff) == 8);
        CHECK(popcount((uint8_t)0xff) == 8);
        CHECK(popcount((uint32_t)0x55555555u) == 16);
        CHECK(popcount((uint64_t)~0ull) == 64);
        static_assert(popcount((uint32_t)7) == 3);
        static_assert(countl_zero((uint32_t)0) == 32);
    }

    /* ---- powers of two ------------------------------------------------- */
    {
        /* Zero is NOT a power of two, and `(x & (x - 1)) == 0` says it
         * is -- which is the bug this function exists to not have. */
        CHECK(!has_single_bit((uint32_t)0));
        CHECK(has_single_bit((uint32_t)1));
        CHECK(has_single_bit((uint32_t)64));
        CHECK(!has_single_bit((uint32_t)3));
        CHECK(!has_single_bit((uint32_t)0xffffffffu));

        CHECK(bit_width((uint32_t)0) == 0);
        CHECK(bit_width((uint32_t)1) == 1);
        CHECK(bit_width((uint32_t)255) == 8);
        CHECK(bit_width((uint32_t)256) == 9);

        /* bit_ceil(0) is 1: an empty table still needs one bucket. */
        CHECK(bit_ceil((uint32_t)0) == 1);
        CHECK(bit_ceil((uint32_t)1) == 1);
        CHECK(bit_ceil((uint32_t)5) == 8);
        CHECK(bit_ceil((uint32_t)8) == 8);
        CHECK(bit_ceil((uint32_t)9) == 16);

        /* bit_floor(0) is 0: there is no such power, and zero is the
         * only honest answer. */
        CHECK(bit_floor((uint32_t)0) == 0);
        CHECK(bit_floor((uint32_t)1) == 1);
        CHECK(bit_floor((uint32_t)5) == 4);
        CHECK(bit_floor((uint32_t)8) == 8);
        static_assert(bit_ceil((uint32_t)100) == 128);
    }

    /* ---- numbers ---------------------------------------------------------- */
    {
        /* A float constant is a FLOAT, not a double rounded on every
         * use -- which is the reason these are variable templates. */
        static_assert(is_same_v<decltype(numbers::pi_v<float>), const float>);
        static_assert(is_same_v<decltype(numbers::pi), const double>);
        CHECK(numbers::pi > 3.14159 && numbers::pi < 3.1416);
        CHECK(numbers::e > 2.71828 && numbers::e < 2.71829);
        CHECK(numbers::pi_v<float> > 3.14f && numbers::pi_v<float> < 3.15f);
        /* Each is the value it claims, to within its type. */
        CHECK(numbers::sqrt2 * numbers::sqrt2 > 1.9999999 &&
              numbers::sqrt2 * numbers::sqrt2 < 2.0000001);
        CHECK(numbers::inv_pi * numbers::pi > 0.9999999);
        CHECK(numbers::ln2 * numbers::log2e > 0.9999999);
        CHECK(numbers::phi * numbers::phi - numbers::phi > 0.9999999 &&
              numbers::phi * numbers::phi - numbers::phi < 1.0000001);
        /* A long double one really is wider. */
        CHECK((long double)numbers::pi_v<long double> != (long double)0);
    }

    /* ---- version: the macros a program branches on ------------------------ */
    {
        /* The point of <version> is that these are testable WITHOUT
         * including the header they describe. */
#if !defined(__cpp_lib_bit_cast)
        CHECK(false);            /* <bit> is here, so this must be defined */
#endif
#if !defined(__cpp_lib_coroutine)
        CHECK(false);
#endif
#if !defined(__cpp_lib_source_location)
        CHECK(false);
#endif
#if !defined(__cpp_lib_math_constants)
        CHECK(false);
#endif
#if defined(__cpp_lib_jthread)
        CHECK(false);            /* threads are NOT here, and must not claim */
#endif
#if defined(__cpp_lib_atomic_wait)
        CHECK(false);
#endif
        CHECK(__cpp_lib_bit_cast >= 201806L);
        CHECK(__cpp_lib_coroutine >= 201902L);
        CHECK(__cpp_lib_ranges >= 201911L);
    }

    DONE();
}
