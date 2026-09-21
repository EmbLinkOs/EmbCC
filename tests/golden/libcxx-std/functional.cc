/* <functional>: INVOKE made callable, and the transparent operators. */
#include <functional>
#include "check.h"
using namespace std;

struct M { int v; int twice() const { return v * 2; } };
static int add3(int a, int b, int c) { return a + b + c; }

int main()
{
    M m{21};
    /* All four INVOKE spellings through one function -- which is why
     * generic code calls invoke() rather than writing f(a...). */
    CHECK(invoke(add3, 1, 2, 3) == 6);
    CHECK(invoke(&M::twice, m) == 42);        /* through an object */
    CHECK(invoke(&M::twice, &m) == 42);       /* through a pointer */
    CHECK(invoke(&M::v, m) == 21);            /* a data member */
    invoke_r<void>(add3, 1, 2, 3);            /* a void result is allowed */

    int x = 10;
    auto r = ref(x);
    r.get() = 99;
    CHECK(x == 99);                           /* it really is a reference */
    CHECK((is_same_v<unwrap_ref_decay_t<decltype(r)>, int &>));
    auto cr = cref(x);
    CHECK(cr.get() == 99);

    /* The transparent form deduces its arguments, so it compares an int
     * with a double without converting either to a fixed type first. */
    CHECK(less<>{}(1, 2.5));
    CHECK(!less<>{}(2.5, 1));
    CHECK(plus<>{}(2, 3) == 5);
    CHECK(negate<>{}(7) == -7);
    CHECK(logical_not<>{}(0));
    CHECK(bit_xor<>{}(6, 3) == 5);
    CHECK(less<int>{}(1, 3) && !less<int>{}(3, 1));
    CHECK(multiplies<int>{}(6, 7) == 42);
    CHECK(greater_equal<int>{}(3, 3));

    auto odd = [](int n) { return n % 2 != 0; };
    CHECK(not_fn(odd)(4));
    CHECK(!not_fn(odd)(3));
    CHECK(identity{}(5) == 5);

    /* A reference_wrapper is callable, forwarding through invoke. */
    auto fr = ref(add3);
    CHECK(fr(1, 2, 3) == 6);
    DONE();
}
