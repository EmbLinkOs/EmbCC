/* <iterator>: the vocabulary that makes one algorithm work on a pointer,
 * a container and a C array alike. */
#include <iterator>
#include "check.h"
using namespace std;

int main()
{
    int a[5] = {1, 2, 3, 4, 5};

    /* A C array is a range: this is what makes `for (x : arr)` work. */
    CHECK(size(a) == 5);
    CHECK(ssize(a) == 5);
    CHECK(!empty(a));
    CHECK(*begin(a) == 1 && *(end(a) - 1) == 5);
    CHECK(data(a) == a);

    CHECK(distance(begin(a), end(a)) == 5);
    int *p = begin(a);
    advance(p, 3);
    CHECK(*p == 4);
    advance(p, -2);
    CHECK(*p == 2);
    CHECK(*next(begin(a), 2) == 3);
    CHECK(*prev(end(a)) == 5);

    /* reverse_iterator holds the base one PAST what it refers to, which
     * is what lets rend() be begin() rather than one before it. */
    int seen[5], n = 0;
    for (auto it = rbegin(a); it != rend(a); ++it)
        seen[n++] = *it;
    CHECK(n == 5);
    CHECK(seen[0] == 5 && seen[4] == 1);
    CHECK(rbegin(a)[2] == 3);
    CHECK(rend(a) - rbegin(a) == 5);
    CHECK(make_reverse_iterator(end(a)).base() == end(a));

    /* A pointer's traits, and the contiguous tag that only a pointer has. */
    CHECK((is_same_v<iter_value_t<int *>, int>));
    CHECK((is_same_v<iter_difference_t<int *>, ptrdiff_t>));
    CHECK((is_base_of_v<random_access_iterator_tag, iter_category_t<int *>>));

    /* iterator_traits must be EMPTY for a non-iterator, not an error --
     * generic code asks about types that are not iterators. */
    struct NotAnIterator {};
    CHECK(!(__detail::__is_cat<NotAnIterator, input_iterator_tag>::value));

    /* move_iterator dereferences to an rvalue, so plain assignment moves. */
    struct Tracked {
        int v;
        Tracked(int x = 0) : v(x) {}
        Tracked(const Tracked &o) : v(o.v) {}
        /* Marks the source, so the test can see that it really moved
         * rather than copied. */
        Tracked(Tracked &&o) noexcept : v(o.v) { o.v = -1; }
    };
    Tracked src[2] = {Tracked(8), Tracked(9)};
    auto m = make_move_iterator(src);
    Tracked dst(*m);
    CHECK(dst.v == 8);
    CHECK(src[0].v == -1);      /* it really was moved from */
    DONE();
}
