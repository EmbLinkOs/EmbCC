/* <ranges>.
 *
 * The checks that matter here are not "did the pipeline produce the
 * right numbers". They are:
 *
 *   LAZINESS. A view touches no element until it is iterated, and then
 *     touches each exactly once. The predicates and transforms below
 *     COUNT their calls, so a pipeline that quietly materialised an
 *     intermediate container, or evaluated the predicate twice per
 *     element, fails even though its output is right.
 *
 *   CATEGORY. filter_view over a vector is bidirectional, not random
 *     access, because you cannot jump n kept elements ahead without
 *     looking at the ones between. An iterator that claimed otherwise
 *     would make std::advance skip the wrong elements silently.
 *
 *   OWNERSHIP. A view holds a pointer to its source and copies cheaply.
 *     Mutating the source through a transform's reference must be
 *     visible in the source.
 */
#include "check.h"
#include <map>
#include <ranges>
#include <string>
#include <vector>

using namespace std;
namespace rv = std::views;

static int pred_calls, xform_calls;

int main()
{
    vector<int> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    /* ---- the range concepts ------------------------------------------- */
    static_assert(ranges::range<vector<int>>);
    static_assert(ranges::range<int[4]>);          /* an array IS a range */
    static_assert(!ranges::range<int>);
    static_assert(ranges::sized_range<vector<int>>);
    static_assert(ranges::random_access_range<vector<int>>);
    static_assert(ranges::contiguous_range<vector<int>>);
    static_assert(ranges::bidirectional_range<map<int, int>>);
    /* A map is bidirectional and NOT random access: its iterator walks a
     * tree, and `it + 3` has no meaning. */
    static_assert(!ranges::random_access_range<map<int, int>>);
    static_assert(ranges::common_range<vector<int>>);

    /* ---- iterating a range, and ranges::begin on an array -------------- */
    {
        int a[4] = {10, 20, 30, 40};
        CHECK(ranges::begin(a) == a && ranges::end(a) == a + 4);
        CHECK(ranges::size(a) == 4);
        CHECK(ranges::size(v) == 10);
    }

    /* ---- filter: laziness, counted ------------------------------------- */
    {
        pred_calls = 0;
        auto odd = [](int x) { pred_calls++; return x % 2 != 0; };
        auto f = v | rv::filter(odd);
        /* Building the pipeline evaluates NOTHING. This is the check the
         * whole header exists for: an implementation that eagerly filled
         * a vector would have called the predicate ten times by now. */
        CHECK(pred_calls == 0);

        int sum = 0, n = 0;
        for (int x : f) { sum += x; n++; }
        CHECK(sum == 1 + 3 + 5 + 7 + 9);
        CHECK(n == 5);
        /* Each element is examined once per pass. begin() skips to the
         * first match and ++ skips to the next; there is no second
         * traversal hiding anywhere. */
        CHECK(pred_calls == 10);
    }

    /* ---- transform: one call per element, and no copy of the source ---- */
    {
        xform_calls = 0;
        auto sq = [](int x) { xform_calls++; return x * x; };
        auto t = v | rv::transform(sq);
        CHECK(xform_calls == 0);
        int sum = 0;
        for (int x : t) sum += x;
        CHECK(sum == 385);                 /* 1+4+9+...+100 */
        CHECK(xform_calls == 10);
    }

    /* ---- composed: each element passes through once -------------------- */
    {
        pred_calls = xform_calls = 0;
        auto even = [](int x) { pred_calls++; return x % 2 == 0; };
        auto ten = [](int x) { xform_calls++; return x * 10; };
        int sum = 0;
        for (int x : v | rv::filter(even) | rv::transform(ten))
            sum += x;
        CHECK(sum == 300);                 /* (2+4+6+8+10) * 10 */
        CHECK(pred_calls == 10);
        /* The transform runs only on what SURVIVES the filter. Two
         * separate passes over intermediate vectors would call it ten
         * times; a fused pipeline calls it five. */
        CHECK(xform_calls == 5);
    }

    /* Order matters, and the counts show why. */
    {
        pred_calls = xform_calls = 0;
        auto ten = [](int x) { xform_calls++; return x * 10; };
        auto big = [](int x) { pred_calls++; return x > 50; };
        int n = 0;
        for (int x : v | rv::transform(ten) | rv::filter(big)) { (void)x; n++; }
        CHECK(n == 5);                     /* 60,70,80,90,100 */
        CHECK(pred_calls == 10);
        /* Transforming first means transforming everything -- ten for
         * the filter's test, and then FIVE MORE for the five survivors
         * the loop body dereferences. A transform_view applies its
         * function per dereference and caches nothing, because caching
         * would mean holding a value and a transform view must stay the
         * size of its parts. It is the one place `filter | transform`
         * and `transform | filter` differ in cost as well as in
         * meaning. */
        CHECK(xform_calls == 15);
    }

    /* ---- the category a filter can honestly claim ---------------------- */
    {
        auto f = v | rv::filter([](int x) { return x % 3 == 0; });
        using FI = decltype(f.begin());
        static_assert(is_same_v<iterator_traits<FI>::iterator_category,
                                bidirectional_iterator_tag>);
        /* A transform keeps what its source had: applying f to element n
         * costs what reaching element n costs. */
        auto t = v | rv::transform([](int x) { return x + 1; });
        using TI = decltype(t.begin());
        static_assert(is_same_v<iterator_traits<TI>::iterator_category,
                                random_access_iterator_tag>);
        /* ... which means the random-access operations really work. */
        CHECK(t.begin()[3] == 5);
        CHECK(*(t.begin() + 9) == 11);
        CHECK(t.end() - t.begin() == 10);

        /* And the filter's bidirectional ones do too. */
        auto i = f.begin();
        CHECK(*i == 3);
        ++i;
        CHECK(*i == 6);
        --i;
        CHECK(*i == 3);
    }

    /* ---- take, drop, and the counts they save -------------------------- */
    {
        xform_calls = 0;
        auto ten = [](int x) { xform_calls++; return x * 10; };
        int sum = 0;
        for (int x : v | rv::transform(ten) | rv::take(3))
            sum += x;
        CHECK(sum == 60);
        /* take(3) evaluates three elements, not ten -- which is the
         * point of it and is invisible in the result alone. */
        CHECK(xform_calls == 3);
    }
    {
        int sum = 0;
        for (int x : v | rv::drop(7)) sum += x;
        CHECK(sum == 8 + 9 + 10);
        /* Taking more than there is takes all of it, rather than
         * running off the end. */
        int n = 0;
        for (int x : v | rv::take(100)) { (void)x; n++; }
        CHECK(n == 10);
        n = 0;
        for (int x : v | rv::drop(100)) { (void)x; n++; }
        CHECK(n == 0);
    }

    /* ---- take_while / drop_while --------------------------------------- */
    {
        int n = 0;
        for (int x : v | rv::take_while([](int a) { return a < 5; })) {
            (void)x;
            n++;
        }
        CHECK(n == 4);
        /* take_while stops at the FIRST failure and does not resume --
         * unlike filter, which skips and continues. */
        vector<int> w{1, 2, 9, 3, 4};
        n = 0;
        for (int x : w | rv::take_while([](int a) { return a < 5; })) {
            (void)x;
            n++;
        }
        CHECK(n == 2);
        int sum = 0;
        for (int x : w | rv::drop_while([](int a) { return a < 5; }))
            sum += x;
        CHECK(sum == 9 + 3 + 4);
    }

    /* ---- reverse -------------------------------------------------------- */
    {
        string s;
        for (int x : v | rv::take(4) | rv::reverse)
            s += (char)('0' + x);
        CHECK(s == "4321");
        int first = *(v | rv::reverse).begin();
        CHECK(first == 10);
    }

    /* ---- iota: the view with no source --------------------------------- */
    {
        int sum = 0;
        for (int x : rv::iota(1, 6)) sum += x;
        CHECK(sum == 15);
        /* An unbounded iota must be stopped by something downstream. */
        sum = 0;
        for (int x : rv::iota(0) | rv::take(5)) sum += x;
        CHECK(sum == 10);
        /* And it composes like anything else. */
        sum = 0;
        for (int x : rv::iota(1, 11)
                   | rv::filter([](int a) { return a % 2 == 0; })
                   | rv::transform([](int a) { return a * a; }))
            sum += x;
        CHECK(sum == 4 + 16 + 36 + 64 + 100);
    }

    /* ---- single and empty ----------------------------------------------- */
    {
        int n = 0, sum = 0;
        for (int x : ranges::single_view<int>(7)) { sum += x; n++; }
        CHECK(n == 1 && sum == 7);
        n = 0;
        for (int x : ranges::empty_view<int>()) { (void)x; n++; }
        CHECK(n == 0);
    }

    /* ---- keys and values over a map -------------------------------------- */
    {
        map<int, string> m{{1, "one"}, {2, "two"}, {3, "three"}};
        int ksum = 0;
        for (int k : m | rv::keys) ksum += k;
        CHECK(ksum == 6);
        string all;
        for (const string &s : m | rv::values) all += s;
        CHECK(all == "onetwothree");     /* a map iterates in key order */
        /* And they compose. */
        int n = 0;
        for (int k : m | rv::keys | rv::filter([](int a) { return a > 1; }))
            n += k;
        CHECK(n == 5);
    }

    /* ---- join: a range of ranges, flattened ------------------------------ */
    {
        vector<vector<int>> vv{{1, 2}, {}, {3}, {}, {4, 5, 6}};
        int sum = 0, n = 0;
        for (int x : vv | rv::join) { sum += x; n++; }
        CHECK(sum == 21 && n == 6);
        /* The empty inner ranges are the case a first attempt gets
         * wrong: the outer iterator must advance REPEATEDLY, not once. */
        vector<vector<int>> ee{{}, {}, {}};
        n = 0;
        for (int x : ee | rv::join) { (void)x; n++; }
        CHECK(n == 0);
        vector<vector<int>> lead{{}, {}, {9}};
        n = 0;
        int got = 0;
        for (int x : lead | rv::join) { got = x; n++; }
        CHECK(n == 1 && got == 9);
    }

    /* ---- a view refers, it does not own ---------------------------------- */
    {
        vector<int> src{1, 2, 3};
        auto r = ranges::ref_view<vector<int>>(src);
        CHECK(r.begin() == src.begin());   /* the same iterators */
        src.push_back(4);
        /* Rebuilt after the source changed: a view that had copied would
         * still show three. (The old iterators are invalid, exactly as
         * they would be for the vector itself -- a view inherits its
         * source's invalidation rules and adds none.) */
        auto r2 = ranges::ref_view<vector<int>>(src);
        int n = 0;
        for (int x : r2) { (void)x; n++; }
        CHECK(n == 4);
    }

    /* Writing through a view writes through to the source. */
    {
        vector<int> src{1, 2, 3, 4};
        for (int &x : src | rv::filter([](int a) { return a % 2 == 0; }))
            x = -x;
        CHECK(src[0] == 1 && src[1] == -2 && src[2] == 3 && src[3] == -4);
    }

    /* ---- subrange: the two worlds meet ------------------------------------ */
    {
        ranges::subrange<vector<int>::iterator> s(v.begin() + 2, v.begin() + 5);
        int sum = 0;
        for (int x : s) sum += x;
        CHECK(sum == 3 + 4 + 5);
        CHECK(s.size() == 3);
        CHECK(!s.empty());
        CHECK(s.front() == 3 && s.back() == 5);
        CHECK(s[1] == 4);
        /* And it pipes, because it is a view. */
        int n = 0;
        for (int x : s | rv::transform([](int a) { return a * 2; })) {
            (void)x;
            n++;
        }
        CHECK(n == 3);
    }

    /* ---- a composed closure is a VALUE ------------------------------------ */
    {
        /* This is what a niebloid-free pipe buys: the adaptor can be
         * named, stored and passed before it ever sees a range. */
        auto evens_doubled = rv::filter([](int a) { return a % 2 == 0; })
                           | rv::transform([](int a) { return a * 2; });
        int sum = 0;
        for (int x : v | evens_doubled) sum += x;
        CHECK(sum == 60);
        /* ... and reused on a different range. */
        vector<int> w{2, 3, 4};
        sum = 0;
        for (int x : w | evens_doubled) sum += x;
        CHECK(sum == 12);
    }

    /* ---- the range-form algorithms ---------------------------------------- */
    {
        CHECK(ranges::count_if(v, [](int x) { return x > 7; }) == 3);
        CHECK(ranges::count(v, 5) == 1);
        CHECK(*ranges::find(v, 7) == 7);
        CHECK(ranges::find(v, 99) == v.end());
        CHECK(*ranges::find_if(v, [](int x) { return x * x > 50; }) == 8);
        CHECK(ranges::all_of(v, [](int x) { return x > 0; }));
        CHECK(!ranges::all_of(v, [](int x) { return x > 1; }));
        CHECK(ranges::any_of(v, [](int x) { return x == 10; }));
        CHECK(ranges::none_of(v, [](int x) { return x > 10; }));

        int total = 0;
        ranges::for_each(v, [&](int x) { total += x; });
        CHECK(total == 55);

        /* An algorithm over a VIEW, which is the composition that makes
         * the range form worth having: no temporary vector anywhere. */
        CHECK(ranges::count_if(v | rv::transform([](int x) { return x % 3; }),
                               [](int x) { return x == 0; }) == 3);
        CHECK(ranges::distance(v | rv::filter([](int x) { return x < 4; }))
              == 3);

        vector<int> u{5, 1, 4, 2, 3};
        ranges::sort(u);
        CHECK(u[0] == 1 && u[4] == 5);
        ranges::sort(u, [](int a, int b) { return a > b; });
        CHECK(u[0] == 5 && u[4] == 1);

        vector<int> dst;
        ranges::copy(v | rv::take(3), back_inserter(dst));
        CHECK(dst.size() == 3 && dst[2] == 3);
    }

    /* ---- an empty source through every adaptor ---------------------------- */
    {
        vector<int> e;
        CHECK(ranges::distance(e | rv::filter([](int) { return true; })) == 0);
        CHECK(ranges::distance(e | rv::transform([](int x) { return x; })) == 0);
        CHECK(ranges::distance(e | rv::take(5)) == 0);
        CHECK(ranges::distance(e | rv::drop(5)) == 0);
        CHECK(ranges::distance(e | rv::reverse) == 0);
    }

    /* ---- a filter that keeps nothing, and one that keeps everything ------- */
    {
        CHECK(ranges::distance(v | rv::filter([](int) { return false; })) == 0);
        CHECK(ranges::distance(v | rv::filter([](int) { return true; })) == 10);
        /* The first element failing is the case begin() must handle: it
         * has to skip before it is ever dereferenced. */
        auto f = v | rv::filter([](int x) { return x > 5; });
        CHECK(*f.begin() == 6);
    }

    DONE();
}
