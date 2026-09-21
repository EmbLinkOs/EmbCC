/* <compare> and the three-way comparison it names.
 *
 * Two things are under test and they are not the same thing:
 *
 *   the HEADER -- that the three categories order, convert and compare
 *     against a literal 0 the way the standard says, and that
 *     common_comparison_category folds to the weakest;
 *
 *   the COMPILER -- that `a <=> b` produces an object of the right
 *     category with the right value, that a defaulted operator<=>
 *     compares members in order, and that declaring one implicitly
 *     declares operator== ([class.compare.default]/2).
 *
 * The second is the part worth writing a test for, because the layout of
 * these classes is an ABI between the compiler and this header: EmbCC
 * writes a signed char at offset 0 and the header reads a signed char at
 * offset 0. Nothing in either file checks the other. A test that
 * constructs an ordering with the compiler and reads it with the header
 * is the only place the two meet.
 */
#include "check.h"
#include <compare>
#include <string>
#include <vector>

using namespace std;

/* Members in declaration order, which is what a defaulted <=> compares
 * and in that order: a differing `a` decides before `b` is looked at. */
struct Point {
    int a;
    int b;
    auto operator<=>(const Point &) const = default;
};

/* A member that is itself compared by a defaulted <=>: the synthesised
 * == has to recurse into Point's synthesised ==, not fall back to a
 * memberwise int compare that would skip Point's own. */
struct Line {
    Point p;
    int c;
    auto operator<=>(const Line &) const = default;
};

/* A user-declared == must WIN over the implicit one; this one is
 * deliberately wrong (everything equal) so that a test seeing `true`
 * proves the implicit declaration stood aside. */
struct Odd {
    int a;
    auto operator<=>(const Odd &) const = default;
    bool operator==(const Odd &) const { return true; }
};

/* Both written out: the implicit declaration must not collide with the
 * explicit defaulted one. */
struct Both {
    int a;
    auto operator<=>(const Both &) const = default;
    bool operator==(const Both &) const = default;
};

/* A weaker category, chosen explicitly: two names equal ignoring case
 * are equivalent and are still different strings, so the promise
 * strong_ordering makes would be false here. */
struct CaseName {
    string s;
    static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
    weak_ordering operator<=>(const CaseName &o) const
    {
        size_t n = s.size() < o.s.size() ? s.size() : o.s.size();
        for (size_t i = 0; i < n; i++) {
            char x = low(s[i]), y = low(o.s[i]);
            if (x != y)
                return x < y ? weak_ordering::less : weak_ordering::greater;
        }
        if (s.size() != o.s.size())
            return s.size() < o.s.size() ? weak_ordering::less
                                         : weak_ordering::greater;
        return weak_ordering::equivalent;
    }
    bool operator==(const CaseName &o) const { return (*this <=> o) == 0; }
};

/* An array of one so the compiler cannot constant-fold the comparison
 * away: these checks are about the code that RUNS. */
static volatile int one = 1;

int main()
{
    /* ---- the categories themselves ----------------------------------- */
    CHECK(strong_ordering::less < 0);
    CHECK(strong_ordering::less <= 0);
    CHECK(!(strong_ordering::less > 0));
    CHECK(strong_ordering::equal == 0);
    CHECK(strong_ordering::greater > 0);
    CHECK(0 < strong_ordering::greater);
    CHECK(weak_ordering::equivalent == 0);
    CHECK(partial_ordering::less < 0);
    /* unordered is the one value that answers NO to all four. */
    CHECK(!(partial_ordering::unordered < 0));
    CHECK(!(partial_ordering::unordered > 0));
    CHECK(!(partial_ordering::unordered == 0));
    CHECK(!(partial_ordering::unordered <= 0));
    CHECK(!(partial_ordering::unordered >= 0));

    /* Weakening converts; strengthening does not, and must not compile
     * -- which a test cannot assert, so it is stated here instead. */
    partial_ordering p = strong_ordering::less;
    CHECK(p < 0);
    weak_ordering w = strong_ordering::greater;
    CHECK(w > 0);

    CHECK(is_eq(strong_ordering::equal));
    CHECK(is_neq(strong_ordering::less));
    CHECK(is_lt(weak_ordering::less));
    CHECK(is_lteq(weak_ordering::equivalent));
    CHECK(is_gt(partial_ordering::greater));
    CHECK(is_gteq(partial_ordering::equivalent));

    /* ---- the compiler's <=> on built-in types ------------------------ */
    /* The ABI between src/cxx/emit.c and <compare>: the value the
     * compiler stores is the value the header reads. */
    CHECK((1 <=> 2) < 0);
    CHECK((2 <=> 2) == 0);
    CHECK((3 <=> 2) > 0);
    CHECK((one <=> 2) < 0);
    static_assert(is_same_v<decltype(1 <=> 2), strong_ordering>);
    static_assert(is_same_v<decltype(1.0 <=> 2.0), partial_ordering>);
    static_assert(is_same_v<decltype('a' <=> 'b'), strong_ordering>);

    /* Floating point is PARTIAL because of the NaN, and this is the
     * whole reason partial_ordering exists. */
    double nan = __builtin_nan("");
    auto fp = 1.0 <=> nan;
    CHECK(!(fp < 0) && !(fp > 0) && !(fp == 0));
    CHECK((1.0 <=> 2.0) < 0);
    CHECK((2.0 <=> 2.0) == 0);

    /* Pointers into one array compare strongly. */
    int arr[3] = {0, 1, 2};
    CHECK((&arr[0] <=> &arr[2]) < 0);
    CHECK((&arr[1] <=> &arr[1]) == 0);

    /* ---- a defaulted <=>, member by member --------------------------- */
    Point x{1, 2}, y{1, 3}, z{2, 0}, x2{1, 2};
    CHECK(x < y);            /* a ties, b decides */
    CHECK(x < z);            /* a decides; b's 2 > 0 is never looked at */
    CHECK(!(z < x));
    CHECK((x <=> x2) == 0);
    static_assert(is_same_v<decltype(x <=> y), strong_ordering>);

    /* The implicitly declared ==. Without [class.compare.default]/2
     * these four lines do not compile at all. */
    CHECK(x == x2);
    CHECK(!(x == y));
    CHECK(x != y);
    CHECK(!(x != x2));

    /* The rewritten forms: `x > y` is `(x <=> y) > 0` and `x != y` is
     * `!(x == y)` -- never `(x <=> y) != 0`. */
    CHECK(y > x);
    CHECK(x <= x2 && x >= x2);

    Line l1{{1, 2}, 3}, l2{{1, 2}, 4}, l3{{1, 3}, 0};
    CHECK(l1 < l2 && l1 < l3);
    CHECK(l1 == l1 && l1 != l2);

    Odd o1{1}, o2{2};
    CHECK(o1 == o2);         /* the user's wrong ==, not an implicit one */
    CHECK(o1 < o2);          /* ... while <=> still orders properly */
    CHECK(!(o1 != o2));      /* != rewrites through the user's == */

    Both b1{1}, b2{1};
    CHECK(b1 == b2 && !(b1 < b2));

    /* ---- a hand-written weak order ----------------------------------- */
    CaseName n1{"Hello"}, n2{"hello"}, n3{"world"};
    CHECK((n1 <=> n2) == 0);
    CHECK(n1 == n2);         /* equivalent ... */
    CHECK(!(n1.s == n2.s));  /* ... and not the same string */
    CHECK(n1 < n3);
    static_assert(is_same_v<decltype(n1 <=> n2), weak_ordering>);

    /* ---- common_comparison_category ---------------------------------- */
    /* The fold takes the WEAKEST: one partial member makes the whole
     * comparison partial however strong the others are. */
    static_assert(is_same_v<common_comparison_category_t<>, strong_ordering>);
    static_assert(is_same_v<common_comparison_category_t<strong_ordering>,
                            strong_ordering>);
    static_assert(is_same_v<common_comparison_category_t<strong_ordering,
                                                         weak_ordering>,
                            weak_ordering>);
    static_assert(is_same_v<common_comparison_category_t<strong_ordering,
                                                         partial_ordering,
                                                         weak_ordering>,
                            partial_ordering>);
    /* Not a category at all: void, not an error. */
    static_assert(is_same_v<common_comparison_category_t<strong_ordering, int>,
                            void>);

    static_assert(is_same_v<compare_three_way_result_t<int>, strong_ordering>);
    static_assert(is_same_v<compare_three_way_result_t<Point>,
                            strong_ordering>);

    /* ---- the function object ----------------------------------------- */
    compare_three_way cmp;
    CHECK(cmp(1, 2) < 0);
    CHECK(cmp(x, y) < 0);
    CHECK(strong_order(1, 2) < 0);
    CHECK(weak_order(n1, n3) < 0);

    /* ---- and it composes with the containers ------------------------- */
    /* A vector of a type with only a defaulted <=> sorts, because sort
     * needs `<` and `<` is a rewrite of <=>. */
    vector<Point> v{{2, 0}, {1, 3}, {1, 2}};
    for (size_t i = 0; i + 1 < v.size(); i++)
        for (size_t j = 0; j + 1 < v.size() - i; j++)
            if (v[j + 1] < v[j]) {
                Point t = v[j];
                v[j] = v[j + 1];
                v[j + 1] = t;
            }
    CHECK(v[0] == (Point{1, 2}));
    CHECK(v[1] == (Point{1, 3}));
    CHECK(v[2] == (Point{2, 0}));

    DONE();
}
