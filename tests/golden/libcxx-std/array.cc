/* <array>: a C array with a container's interface and no overhead. */
#include <array>
#include <stdexcept>
#include "check.h"
using namespace std;

int main()
{
    array<int, 4> a{{1, 2, 3, 4}};
    /* It IS the array: an aggregate with one member and nothing else. */
    static_assert(sizeof(a) == 4 * sizeof(int), "no overhead");

    CHECK(a.size() == 4 && !a.empty());
    CHECK(a.front() == 1 && a.back() == 4);
    CHECK(a.at(2) == 3 && a[2] == 3);
    CHECK(a.data() == &a[0]);

    int sum = 0;
    for (int v : a) sum += v;
    CHECK(sum == 10);
    int rev = 0;
    for (auto it = a.rbegin(); it != a.rend(); ++it) rev = rev * 10 + *it;
    CHECK(rev == 4321);

    auto [p, q, r, s] = a;                    /* structured bindings */
    CHECK(p == 1 && q == 2 && r == 3 && s == 4);
    CHECK(get<1>(a) == 2);
    CHECK(tuple_size<decltype(a)>::value == 4);

    array<int, 4> b = a;
    CHECK(a == b);
    CHECK(!(a < b));
    b[3] = 99;
    CHECK(a != b && a < b && b > a);

    a.fill(7);
    CHECK(a[0] == 7 && a[3] == 7);
    a.swap(b);
    CHECK(a[3] == 99 && b[3] == 7);

    /* N == 0 is legal, and its data() is the one pointer that is not the
     * address of an element -- because there is none. */
    array<int, 0> z;
    CHECK(z.size() == 0 && z.empty());
    CHECK(z.begin() == z.end());
    CHECK(z.data() == nullptr);

    /* at() checks and operator[] does not: that is the whole difference. */
    int caught = 0;
    const char *msg = "";
    try { (void)a.at(9); }
    catch (const out_of_range &e) { caught = 1; msg = e.what(); }
    CHECK(caught == 1);
    CHECK(msg[0] == 'a');                     /* "array::at: ..." */

    /* Catching by VALUE copies the exception, and that copy must not
     * throw -- a copy constructor that allocates during unwinding is
     * terminate(). The message survives the copy. */
    int byval = 0;
    try { throw length_error("too long"); }
    catch (logic_error e) { byval = e.what()[0] == 't'; }
    CHECK(byval == 1);

    /* The hierarchy really is a hierarchy. */
    int asbase = 0;
    try { throw range_error("r"); }
    catch (const runtime_error &) { asbase = 1; }
    CHECK(asbase == 1);
    int notlogic = 0;
    try { throw range_error("r"); }
    catch (const logic_error &) { notlogic = 1; }
    catch (const runtime_error &) { notlogic = 2; }
    CHECK(notlogic == 2);                     /* runtime, not logic */

    int c[3] = {5, 6, 7};
    auto t = to_array(c);
    CHECK(t.size() == 3 && t[2] == 7);
    DONE();
}
