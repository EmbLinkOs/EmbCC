/* <any> and <concepts>.
 *
 * Two headers that look unrelated and are the two halves of the same
 * question: what does a type have to promise, and what can you do with
 * a value whose type you have forgotten?
 *
 * <any> is checked by COUNTING. A test that stores a value and reads it
 * back proves nothing about the hard part -- an `any` that leaked every
 * value it held would pass it. So the type stored here counts its own
 * constructions and destructions, and the checks are that the counts
 * balance: every copy destroyed, every move leaving its source empty,
 * and nothing left alive when the scope ends.
 *
 * <concepts> is checked at compile time with static_assert, and the
 * checks that matter are the NEGATIVE ones. A concept that is satisfied
 * by everything compiles every program and constrains nothing.
 */
#include "check.h"
#include <any>
#include <compare>
#include <concepts>
#include <string>
#include <vector>

using namespace std;

/* ---- the counted type -------------------------------------------------
 * Three words of payload, so it fits <any>'s small buffer; the guard
 * value catches a byte-copy that moved the object without telling it. */
static int live, ctors, copies, moves, dtors;
struct Counted {
    long a, b, c;
    Counted(long v = 0) : a(v), b(v + 1), c(v + 2) { live++; ctors++; }
    Counted(const Counted &o) : a(o.a), b(o.b), c(o.c)
    { live++; copies++; }
    Counted(Counted &&o) noexcept : a(o.a), b(o.b), c(o.c)
    { o.a = -1; live++; moves++; }
    ~Counted() { live--; dtors++; }
    bool ok() const { return b == a + 1 && c == a + 2; }
};
static void reset_counts() { live = ctors = copies = moves = dtors = 0; }

/* Too big for the buffer: this one must go on the heap, and moving the
 * any must then move a POINTER rather than the object. */
struct Big {
    long pad[16];
    Big(long v = 0) { for (int i = 0; i < 16; i++) pad[i] = v + i; live++; }
    Big(const Big &o) { for (int i = 0; i < 16; i++) pad[i] = o.pad[i];
                        live++; copies++; }
    Big(Big &&o) noexcept { for (int i = 0; i < 16; i++) pad[i] = o.pad[i];
                            live++; moves++; }
    ~Big() { live--; }
};

/* A move that may throw: the standard requires this to live out of line
 * even though it would fit, because an any's own move is noexcept. */
struct ThrowingMove {
    long v;
    ThrowingMove(long x = 0) : v(x) {}
    ThrowingMove(const ThrowingMove &) = default;
    ThrowingMove(ThrowingMove &&o) : v(o.v) {}   /* not noexcept */
};

/* ---- types for the concept checks ------------------------------------ */
struct Base {};
struct Derived : Base {};
struct Unrelated {};
struct NoDefault { NoDefault() = delete; NoDefault(int) {} };
struct NoCopy {
    NoCopy() = default;
    NoCopy(const NoCopy &) = delete;
    NoCopy &operator=(const NoCopy &) = delete;
    NoCopy(NoCopy &&) = default;
    NoCopy &operator=(NoCopy &&) = default;
};
struct Eq { int v; bool operator==(const Eq &o) const { return v == o.v; } };
struct Ord {
    int v;
    auto operator<=>(const Ord &) const = default;
};
/* An `==` whose result is not boolean-testable. Returning `int` would
 * NOT do: an int converts to bool and `!` of one is a bool, so int is
 * boolean-testable and a type comparing to int is properly comparable.
 * It takes a type with no conversion at all to fail the concept. */
struct Opaque {};
struct NotBool { int v; Opaque operator==(const NotBool &) const { return {}; } };

int main()
{
    /* ==== <concepts> ==================================================
     * Each pair is one positive and one negative: a concept is only
     * worth having if something fails it. */
    static_assert(same_as<int, int> && !same_as<int, const int>);
    static_assert(derived_from<Derived, Base>);
    static_assert(!derived_from<Base, Derived>);
    static_assert(!derived_from<Unrelated, Base>);
    /* derived_from is not convertible_to: a private base converts
     * nowhere, and an int converts to long while deriving from nothing. */
    static_assert(convertible_to<int, long> && !derived_from<int, long>);
    static_assert(!convertible_to<Base *, Derived *>);

    static_assert(integral<int> && integral<char> && integral<bool>);
    static_assert(!integral<float> && !integral<int *>);
    static_assert(signed_integral<long> && !signed_integral<unsigned>);
    static_assert(unsigned_integral<unsigned char>);
    static_assert(!unsigned_integral<signed char>);
    static_assert(floating_point<double> && !floating_point<int>);

    static_assert(destructible<int> && destructible<string>);
    static_assert(constructible_from<Counted, long>);
    static_assert(!constructible_from<NoDefault>);
    static_assert(constructible_from<NoDefault, int>);
    static_assert(default_initializable<int> && !default_initializable<NoDefault>);

    static_assert(move_constructible<NoCopy> && !copy_constructible<NoCopy>);
    static_assert(copy_constructible<string> && copy_constructible<int>);
    static_assert(movable<NoCopy> && !copyable<NoCopy>);
    static_assert(copyable<string> && semiregular<string>);
    static_assert(regular<int> && regular<Eq>);
    /* NoCopy is not regular, and neither is a type with no ==. */
    static_assert(!regular<NoCopy>);
    static_assert(!equality_comparable<Base>);

    static_assert(equality_comparable<Eq> && equality_comparable<int>);
    static_assert(equality_comparable_with<int, long>);
    /* boolean-testable is what rules out an == that does not answer a
     * yes-or-no question at all. */
    static_assert(!equality_comparable<NotBool>);

    /* three_way_comparable, and the CATEGORY is the point: Ord's
     * defaulted <=> over an int is strong, so it satisfies the concept
     * at every category; a weaker one satisfies only the weaker ones. */
    static_assert(three_way_comparable<int>);
    static_assert(three_way_comparable<int, strong_ordering>);
    static_assert(three_way_comparable<Ord, strong_ordering>);
    static_assert(three_way_comparable<Ord, weak_ordering>);
    static_assert(three_way_comparable<double>);
    /* A double's <=> is partial, and partial is not strong. */
    static_assert(!three_way_comparable<double, strong_ordering>);
    /* Eq has == and no ordering at all. */
    static_assert(!three_way_comparable<Eq>);
    static_assert(three_way_comparable_with<int, long>);

    static_assert(totally_ordered<int> && totally_ordered<string>);
    /* Ord gets its <, >, <= and >= from the defaulted <=>, rewritten. */
    static_assert(totally_ordered<Ord>);
    static_assert(!totally_ordered<Eq>);   /* == only, no ordering */

    static_assert(assignable_from<int &, int>);
    static_assert(!assignable_from<int, int>);   /* not an lvalue ref */
    static_assert(!assignable_from<NoCopy &, const NoCopy &>);

    static_assert(swappable<int> && swappable<string>);

    auto twice = [](int x) { return x * 2; };
    auto is_odd = [](int x) { return x % 2 != 0; };
    static_assert(invocable<decltype(twice), int>);
    static_assert(!invocable<decltype(twice), string>);
    static_assert(predicate<decltype(is_odd), int>);
    /* ... and a predicate is about the RESULT: `twice` returning int is
     * still a predicate, because an int is boolean-testable. One
     * returning a type with no conversion to bool is not. */
    static_assert(predicate<decltype(twice), int>);
    static_assert(!predicate<decltype([](int) { return Opaque{}; }), int>);
    static_assert(relation<decltype([](int a, int b) { return a < b; }),
                           int, int>);
    CHECK(twice(21) == 42 && is_odd(3));

    /* Subsumption -- the reason concepts exist rather than enable_if.
     * Both overloads are viable for a long; the more constrained one
     * wins because signed_integral is spelled as integral && ... */
    struct Pick {
        static int f(integral auto) { return 1; }
        static int f(signed_integral auto) { return 2; }
    };
    CHECK(Pick::f(1L) == 2);
    CHECK(Pick::f(1U) == 1);

    /* ==== <any> =======================================================
     * The empty state first: every operation on it must be defined. */
    {
        any e;
        CHECK(!e.has_value());
        CHECK(e.type() == typeid(void));
        CHECK(any_cast<int>(&e) == nullptr);
        e.reset();                       /* on an empty any: a no-op */
        CHECK(!e.has_value());
        any e2 = e;                      /* copying empty stays empty */
        CHECK(!e2.has_value());
        bool threw = false;
        try { (void)any_cast<int>(e); } catch (const bad_any_cast &) { threw = true; }
        CHECK(threw);
    }

    /* A small trivial value: no allocation, and it round-trips. */
    {
        any a = 42;
        CHECK(a.has_value() && a.type() == typeid(int));
        CHECK(any_cast<int>(a) == 42);
        CHECK(*any_cast<int>(&a) == 42);
        /* The cast is on type IDENTITY: an int is not a long. */
        CHECK(any_cast<long>(&a) == nullptr);
        CHECK(any_cast<unsigned>(&a) == nullptr);
        *any_cast<int>(&a) = 7;
        CHECK(any_cast<int>(a) == 7);
        a = string("hello");             /* and it can change type */
        CHECK(a.type() == typeid(string));
        CHECK(any_cast<string>(a) == "hello");
    }

    /* ---- the counting part: every value is destroyed ---------------- */
    reset_counts();
    {
        any a = Counted(10);
        CHECK(live == 1);
        CHECK(any_cast<Counted>(&a)->a == 10);
        CHECK(any_cast<Counted>(&a)->ok());
    }
    CHECK(live == 0);                    /* the any destroyed it */
    CHECK(dtors == ctors + copies + moves);

    /* Copying an any copies the value -- two independent objects. */
    reset_counts();
    {
        any a = Counted(1);
        any b = a;
        CHECK(live == 2);
        CHECK(copies == 1);
        any_cast<Counted>(&b)->a = 99;
        CHECK(any_cast<Counted>(&a)->a == 1);   /* not aliased */
        CHECK(any_cast<Counted>(&b)->a == 99);
    }
    CHECK(live == 0);

    /* Moving an any leaves the source EMPTY. */
    reset_counts();
    {
        any a = Counted(5);
        any b = static_cast<any &&>(a);
        CHECK(!a.has_value());           /* the standard requires this */
        CHECK(b.has_value());
        CHECK(any_cast<Counted>(&b)->a == 5);
        CHECK(any_cast<Counted>(&b)->ok());
        CHECK(live == 1);
    }
    CHECK(live == 0);

    /* Assigning over a held value destroys it first. */
    reset_counts();
    {
        any a = Counted(1);
        CHECK(live == 1);
        a = 17;                          /* the Counted must die here */
        CHECK(live == 0);
        CHECK(any_cast<int>(a) == 17);
        a.reset();
        CHECK(!a.has_value());
    }
    CHECK(live == 0);

    /* swap: the values change places and stay valid. A memcpy of the
     * buffers would pass the first two checks and fail ok(). */
    reset_counts();
    {
        any a = Counted(100), b = Counted(200);
        a.swap(b);
        CHECK(any_cast<Counted>(&a)->a == 200);
        CHECK(any_cast<Counted>(&b)->a == 100);
        CHECK(any_cast<Counted>(&a)->ok() && any_cast<Counted>(&b)->ok());
        swap(a, b);
        CHECK(any_cast<Counted>(&a)->a == 100);
        CHECK(live == 2);
        any empty;
        a.swap(empty);                   /* swapping with empty */
        CHECK(!a.has_value() && empty.has_value());
        CHECK(any_cast<Counted>(&empty)->a == 100);
    }
    CHECK(live == 0);

    /* The heap path: too big for the buffer, and it must still balance. */
    reset_counts();
    {
        /* Storing it moves it once, out of the temporary and onto the
         * heap; the temporary then dies, so one is left alive. */
        any a = Big(3);
        CHECK(live == 1 && moves == 1 && copies == 0);
        CHECK(a.type() == typeid(Big));
        CHECK(any_cast<Big>(&a)->pad[5] == 8);
        any b = a;
        CHECK(live == 2 && copies == 1);
        /* Moving the ANY moves a POINTER: the 128-byte Big is not
         * touched, which is the whole reason a large value goes out of
         * line. */
        any c = static_cast<any &&>(b);
        CHECK(copies == 1 && moves == 1);
        CHECK(!b.has_value());
        CHECK(any_cast<Big>(&c)->pad[15] == 18);
    }
    CHECK(live == 0);

    /* A throwing move goes out of line even though it fits, so that
     * any's own move can be noexcept. Observable only as: it still
     * works, and the any is still nothrow-movable. */
    {
        static_assert(is_nothrow_move_constructible_v<any>);
        any a = ThrowingMove(8);
        any b = static_cast<any &&>(a);
        CHECK(any_cast<ThrowingMove>(&b)->v == 8);
    }

    /* emplace and make_any construct in place -- no copy, no move. */
    reset_counts();
    {
        any a;
        Counted &r = a.emplace<Counted>(4L);
        CHECK(ctors == 1 && copies == 0 && moves == 0);
        CHECK(r.a == 4 && any_cast<Counted>(&a) == &r);
        a.emplace<Counted>(9L);          /* replaces, destroying the old */
        CHECK(live == 1);
        CHECK(any_cast<Counted>(&a)->a == 9);
    }
    CHECK(live == 0);

    {
        any a = make_any<string>(5u, 'x');
        CHECK(any_cast<string>(a) == "xxxxx");
        any b = make_any<vector<int>>({1, 2, 3});
        CHECK(any_cast<vector<int>>(&b)->size() == 3);
        CHECK((*any_cast<vector<int>>(&b))[2] == 3);
    }

    /* A derived value is not its base: the bytes are a Derived and the
     * offset to the Base subobject is recorded nowhere. */
    {
        any a = Derived{};
        CHECK(a.type() == typeid(Derived));
        CHECK(any_cast<Base>(&a) == nullptr);
    }

    /* The reference form of any_cast, and moving out of one. */
    reset_counts();
    {
        any a = Counted(77);
        Counted &r = any_cast<Counted &>(a);
        r.a = 78;
        CHECK(any_cast<Counted>(&a)->a == 78);
        /* One move already happened putting the value into the any, so
         * taking it out is the second. */
        CHECK(moves == 1);
        Counted taken = any_cast<Counted>(static_cast<any &&>(a));
        CHECK(taken.a == 78 && moves == 2);
        CHECK(a.has_value());            /* moved FROM, not emptied */
    }
    CHECK(live == 0);

    /* And a container of them, which is the whole use for the thing. */
    reset_counts();
    {
        vector<any> v;
        v.push_back(1);
        v.push_back(string("two"));
        v.push_back(Counted(3));
        v.push_back(any{});
        CHECK(v.size() == 4);
        CHECK(any_cast<int>(v[0]) == 1);
        CHECK(any_cast<string>(v[1]) == "two");
        CHECK(any_cast<Counted>(&v[2])->a == 3);
        CHECK(!v[3].has_value());
        v.clear();
    }
    CHECK(live == 0);

    DONE();
}
