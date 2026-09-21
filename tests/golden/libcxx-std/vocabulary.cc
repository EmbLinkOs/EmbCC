/* <tuple>, <optional>, <numeric>: the vocabulary types and the numeric
 * folds. */
#include <tuple>
#include <optional>
#include <numeric>
#include <vector>
#include <string>
#include <functional>
#include "check.h"
using namespace std;

struct Empty1 {}; struct Empty2 {};
static int live;
struct R { int v; R(int x = 0) : v(x) { live++; } R(const R &o) : v(o.v) { live++; }
           R(R &&o) noexcept : v(o.v) { live++; o.v = -1; }
           R &operator=(const R &o) { v = o.v; return *this; }
           R &operator=(R &&o) noexcept { v = o.v; o.v = -1; return *this; }
           ~R() { live--; } };

int main()
{
    /* ---- tuple ---- */
    {
        tuple<int, char, double> t(1, 'x', 2.5);
        CHECK(get<0>(t) == 1 && get<1>(t) == 'x' && get<2>(t) == 2.5);
        CHECK(tuple_size<decltype(t)>::value == 3);
        CHECK((is_same_v<tuple_element<1, decltype(t)>::type, char>));
        get<0>(t) = 9;
        CHECK(get<0>(t) == 9);

        /* Two elements of the SAME type must still be distinct: without
         * the index in the base's type this would be ambiguous. */
        tuple<int, int> same(4, 7);
        CHECK(get<0>(same) == 4 && get<1>(same) == 7);

        /* Empty types cost nothing: the elements are bases, so the empty
         * base optimisation applies. A chain of members would be 2 bytes. */
        CHECK(sizeof(tuple<Empty1, Empty2>) <= sizeof(tuple<Empty1>) + 1);

        auto m = make_tuple(1, string("hi"));
        CHECK((is_same_v<decltype(m), tuple<int, string>>));
        int a = 0; string b;
        tie(a, b) = m;                  /* assigns THROUGH to a and b */
        CHECK(a == 1 && b == "hi");
        int only = 0;
        tie(only, ignore) = m;
        CHECK(only == 1);

        /* make_tuple decays, and a reference_wrapper becomes a real
         * reference -- which is what ref() at a call site is for. */
        int r = 5;
        auto wrapped = make_tuple(ref(r));
        CHECK((is_same_v<decltype(wrapped), tuple<int &>>));
        get<0>(wrapped) = 6;
        CHECK(r == 6);

        CHECK(make_tuple(1, 2) == make_tuple(1, 2));
        CHECK(make_tuple(1, 2) < make_tuple(1, 3));
        CHECK(make_tuple(2, 0) > make_tuple(1, 9));

        auto sum3 = [](int x, int y, int z) { return x + y + z; };
        CHECK(apply(sum3, make_tuple(1, 2, 3)) == 6);

        tuple<int, int> s1(1, 2), s2(3, 4);
        s1.swap(s2);
        CHECK(get<0>(s1) == 3 && get<0>(s2) == 1);
    }

    /* ---- optional ---- */
    {
        optional<int> o;
        CHECK(!o && !o.has_value());
        CHECK(o.value_or(7) == 7);
        o = 5;
        CHECK(o && *o == 5 && o.value() == 5 && o.value_or(7) == 5);
        o.reset();
        CHECK(!o);

        optional<string> s("hello");
        CHECK(s->size() == 5);
        CHECK(*s == "hello");

        /* value() checks, operator* does not -- the asymmetry that keeps
         * the type free in the case it exists for. */
        optional<int> e;
        int caught = 0;
        try { (void)e.value(); } catch (const bad_optional_access &) { caught = 1; }
        CHECK(caught == 1);

        /* The T lives INSIDE the optional: no allocation. */
        CHECK(sizeof(optional<int>) >= sizeof(int));
        CHECK(sizeof(optional<int>) <= 2 * sizeof(int) + sizeof(void *));

        /* Every construction is matched by a destruction. */
        {
            optional<R> a(in_place, 3);
            CHECK(live == 1 && a->v == 3);
            optional<R> b = a;
            CHECK(live == 2 && b->v == 3);
            a.reset();
            CHECK(live == 1);
            b.emplace(9);
            CHECK(live == 1 && b->v == 9);
        }
        CHECK(live == 0);

        /* Moving leaves the source ENGAGED with a moved-from value --
         * [optional.ctor] says so, and clearing it would silently change
         * `if (o)` after a move. */
        {
            optional<R> a(in_place, 4);
            optional<R> b = std::move(a);
            CHECK(b.has_value() && b->v == 4);
            CHECK(a.has_value());
        }
        CHECK(live == 0);

        /* A disengaged optional is less than any value, and equal to
         * another disengaged one. */
        optional<int> n1, n2, v1(1), v2(2);
        CHECK(n1 == n2);
        CHECK(n1 < v1 && !(v1 < n1));
        CHECK(v1 < v2);
        CHECK(n1 == nullopt && v1 != nullopt);
        CHECK(v1 == 1);

        optional<int> x(1), y;
        x.swap(y);
        CHECK(!x && y && *y == 1);

        auto mo = make_optional<string>(3, 'z');
        CHECK(*mo == "zzz");
    }

    /* ---- numeric ---- */
    {
        vector<int> v{1, 2, 3, 4};
        CHECK(accumulate(v.begin(), v.end(), 0) == 10);
        CHECK(accumulate(v.begin(), v.end(), 1, multiplies<>{}) == 24);

        /* init sets the accumulator's TYPE: an int init over doubles
         * truncates every addition. Specified, and the commonest misuse. */
        vector<double> d{0.5, 0.5, 0.5};
        CHECK(accumulate(d.begin(), d.end(), 0) == 0);
        CHECK(accumulate(d.begin(), d.end(), 0.0) == 1.5);

        vector<int> a{1, 2, 3}, b{4, 5, 6};
        CHECK(inner_product(a.begin(), a.end(), b.begin(), 0) == 32);

        vector<int> out(4);
        partial_sum(v.begin(), v.end(), out.begin());
        CHECK(out[0] == 1 && out[3] == 10);
        adjacent_difference(out.begin(), out.end(), out.begin());
        CHECK(out[0] == 1 && out[1] == 2 && out[3] == 4);   /* in place */

        vector<int> seq(5);
        iota(seq.begin(), seq.end(), 10);
        CHECK(seq[0] == 10 && seq[4] == 14);

        CHECK(gcd(12, 18) == 6);
        CHECK(gcd(-4, 6) == 2);          /* absolute values */
        CHECK(lcm(4, 6) == 12);
        CHECK(lcm(0, 5) == 0);

        /* midpoint must not overflow, which (a+b)/2 does. */
        CHECK(midpoint(2, 6) == 4);
        int big = 2000000000;
        CHECK(midpoint(big, big + 100) == big + 50);
        CHECK(midpoint(1.0, 2.0) == 1.5);
    }
    DONE();
}
