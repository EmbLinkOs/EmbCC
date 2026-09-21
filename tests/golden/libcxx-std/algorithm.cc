/* <algorithm>: the guarantees, not just the results. */
#include <algorithm>
#include <vector>
#include <string>
#include "check.h"
using namespace std;

static bool is_even(int x) { return x % 2 == 0; }

int main()
{
    vector<int> v{5, 3, 9, 1, 7, 3};

    /* non-modifying */
    CHECK(count(v.begin(), v.end(), 3) == 2);
    CHECK(count_if(v.begin(), v.end(), is_even) == 0);
    CHECK(*find(v.begin(), v.end(), 9) == 9);
    CHECK(find(v.begin(), v.end(), 42) == v.end());
    CHECK(*find_if(v.begin(), v.end(), [](int x) { return x > 6; }) == 9);
    CHECK(all_of(v.begin(), v.end(), [](int x) { return x > 0; }));
    CHECK(any_of(v.begin(), v.end(), [](int x) { return x == 7; }));
    CHECK(none_of(v.begin(), v.end(), is_even));
    CHECK(*min_element(v.begin(), v.end()) == 1);
    CHECK(*max_element(v.begin(), v.end()) == 9);

    int sum = 0;
    for_each(v.begin(), v.end(), [&](int x) { sum += x; });
    CHECK(sum == 28);

    /* BOTH min and max return the first argument when the two are
     * equivalent ([alg.min.max]), and max_element returns the FIRST
     * maximum. Only identity can see this -- every check of the value
     * passes either way -- and algorithms built on them inherit it. */
    {
        int a = 1, b = 1;
        CHECK(&min(a, b) == &a);
        CHECK(&max(a, b) == &a);
        int tie[] = {3, 1, 3};
        CHECK(max_element(tie, tie + 3) == tie);
        CHECK(min_element(tie, tie + 3) == tie + 1);
        CHECK(min({4, 2, 7}) == 2 && max({4, 2, 7}) == 7);
        CHECK(clamp(5, 1, 3) == 3 && clamp(0, 1, 3) == 1 && clamp(2, 1, 3) == 2);
    }

    /* modifying */
    {
        vector<int> a{1, 2, 3, 4, 5}, b(5);
        copy(a.begin(), a.end(), b.begin());
        CHECK(b == a);
        vector<int> c(5, 0);
        auto e = copy_if(a.begin(), a.end(), c.begin(), is_even);
        CHECK(e - c.begin() == 2 && c[0] == 2 && c[1] == 4);
        transform(a.begin(), a.end(), b.begin(), [](int x) { return x * 10; });
        CHECK(b[4] == 50);
        fill(b.begin(), b.end(), 7);
        CHECK(b[0] == 7 && b[4] == 7);
        replace(a.begin(), a.end(), 3, 30);
        CHECK(a[2] == 30);
        reverse(a.begin(), a.end());
        CHECK(a[0] == 5 && a[4] == 1);
        rotate(a.begin(), a.begin() + 2, a.end());
        CHECK(a.size() == 5);
    }

    /* Overlapping ranges: copy_backward handles what copy cannot. */
    {
        vector<int> a{1, 2, 3, 4, 5};
        copy_backward(a.begin(), a.begin() + 4, a.end());
        CHECK(a[1] == 1 && a[4] == 4);
    }

    /* remove COMPACTS and returns the new end; the container still has to
     * be told. That is the erase-remove idiom. */
    {
        vector<int> a{1, 2, 3, 2, 4, 2};
        auto e = remove(a.begin(), a.end(), 2);
        CHECK(e - a.begin() == 3);
        a.erase(e, a.end());
        CHECK(a.size() == 3 && a[0] == 1 && a[1] == 3 && a[2] == 4);
    }

    /* unique removes ADJACENT duplicates only, which is why it follows
     * a sort. */
    {
        vector<int> a{1, 1, 2, 2, 2, 3, 1};
        auto e = unique(a.begin(), a.end());
        a.erase(e, a.end());
        CHECK(a.size() == 4);                /* the trailing 1 survives */
        CHECK(a[0] == 1 && a[1] == 2 && a[2] == 3 && a[3] == 1);
    }

    /* sorting */
    {
        vector<int> a{5, 3, 9, 1, 7, 3, 8, 2, 6, 4};
        sort(a.begin(), a.end());
        CHECK(is_sorted(a.begin(), a.end()));
        CHECK(a.front() == 1 && a.back() == 9);
        sort(a.begin(), a.end(), greater<int>{});
        CHECK(a.front() == 9 && a.back() == 1);
    }

    /* The worst cases a naive quicksort turns quadratic: already sorted,
     * reversed, and all equal. Introsort has to finish all three, and
     * correctly. */
    {
        const int N = 2000;
        vector<int> a, b, c;
        for (int i = 0; i < N; i++) { a.push_back(i); b.push_back(N - i); c.push_back(7); }
        sort(a.begin(), a.end());
        sort(b.begin(), b.end());
        sort(c.begin(), c.end());
        CHECK(is_sorted(a.begin(), a.end()) && a[0] == 0 && a[N - 1] == N - 1);
        CHECK(is_sorted(b.begin(), b.end()) && b[0] == 1 && b[N - 1] == N);
        CHECK(is_sorted(c.begin(), c.end()) && c[0] == 7 && c[N - 1] == 7);
        /* A pattern that defeats a median-of-three pivot less kindly. */
        vector<int> d;
        for (int i = 0; i < N; i++) d.push_back((i * 7919) % 1021);
        sort(d.begin(), d.end());
        CHECK(is_sorted(d.begin(), d.end()));
        CHECK((int)d.size() == N);
    }

    /* Sorting something with a real move: strings through a vector. */
    {
        vector<string> s{"pear", "apple", "fig", "banana"};
        sort(s.begin(), s.end());
        CHECK(s[0] == "apple" && s[1] == "banana" && s[2] == "fig");
        CHECK(s[3] == "pear");
    }

    /* binary search on a sorted range */
    {
        vector<int> a{1, 3, 3, 3, 5, 7};
        CHECK(binary_search(a.begin(), a.end(), 5));
        CHECK(!binary_search(a.begin(), a.end(), 4));
        CHECK(lower_bound(a.begin(), a.end(), 3) - a.begin() == 1);
        CHECK(upper_bound(a.begin(), a.end(), 3) - a.begin() == 4);
        auto r = equal_range(a.begin(), a.end(), 3);
        CHECK(r.second - r.first == 3);
        CHECK(lower_bound(a.begin(), a.end(), 0) == a.begin());
        CHECK(lower_bound(a.begin(), a.end(), 9) == a.end());
    }

    /* heaps */
    {
        vector<int> a{3, 1, 4, 1, 5, 9, 2, 6};
        make_heap(a.begin(), a.end());
        CHECK(is_heap(a.begin(), a.end()));
        CHECK(a.front() == 9);
        pop_heap(a.begin(), a.end());
        CHECK(a.back() == 9);
        a.pop_back();
        CHECK(is_heap(a.begin(), a.end()) && a.front() == 6);
        a.push_back(100);
        push_heap(a.begin(), a.end());
        CHECK(a.front() == 100);
        sort_heap(a.begin(), a.end());
        CHECK(is_sorted(a.begin(), a.end()));
    }

    /* partitioning */
    {
        vector<int> a{1, 2, 3, 4, 5, 6};
        auto m = partition(a.begin(), a.end(), is_even);
        CHECK(m - a.begin() == 3);
        CHECK(is_partitioned(a.begin(), a.end(), is_even));
        for (auto it = a.begin(); it != m; ++it) CHECK(is_even(*it));
        vector<int> b{2, 4, 6, 1, 3};
        CHECK(partition_point(b.begin(), b.end(), is_even) - b.begin() == 3);
    }

    /* merging and set operations, on sorted ranges */
    {
        vector<int> a{1, 3, 5}, b{2, 3, 6}, out(6);
        auto e = merge(a.begin(), a.end(), b.begin(), b.end(), out.begin());
        CHECK(e - out.begin() == 6);
        CHECK(is_sorted(out.begin(), out.end()));
        vector<int> u(6), i(6), d(6);
        auto ue = set_union(a.begin(), a.end(), b.begin(), b.end(), u.begin());
        CHECK(ue - u.begin() == 5);          /* 3 appears once */
        auto ie = set_intersection(a.begin(), a.end(), b.begin(), b.end(), i.begin());
        CHECK(ie - i.begin() == 1 && i[0] == 3);
        auto de = set_difference(a.begin(), a.end(), b.begin(), b.end(), d.begin());
        CHECK(de - d.begin() == 2 && d[0] == 1 && d[1] == 5);
        CHECK(includes(out.begin(), out.end(), a.begin(), a.end()));
    }

    /* comparison of ranges */
    {
        vector<int> a{1, 2, 3}, b{1, 2, 3}, c{1, 2};
        CHECK(equal(a.begin(), a.end(), b.begin()));
        CHECK(!equal(a.begin(), a.end(), c.begin(), c.end()));  /* lengths */
        CHECK(lexicographical_compare(c.begin(), c.end(), a.begin(), a.end()));
        auto m = mismatch(a.begin(), a.end(), b.begin());
        CHECK(m.first == a.end());
    }

    /* searching for a subrange */
    {
        vector<int> h{1, 2, 3, 4, 5}, n{3, 4};
        CHECK(search(h.begin(), h.end(), n.begin(), n.end()) - h.begin() == 2);
        vector<int> miss{4, 3};
        CHECK(search(h.begin(), h.end(), miss.begin(), miss.end()) == h.end());
        CHECK(adjacent_find(h.begin(), h.end()) == h.end());
        vector<int> dup{1, 2, 2, 3};
        CHECK(adjacent_find(dup.begin(), dup.end()) - dup.begin() == 1);
    }
    DONE();
}
