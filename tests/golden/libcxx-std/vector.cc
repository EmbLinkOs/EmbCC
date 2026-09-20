/* <vector>: three pointers, a growth policy, and exception safety. */
#include <vector>
#include <stdexcept>
#include "check.h"
using namespace std;

static int live;
struct R {
    int v;
    R(int x = 0) : v(x) { live++; }
    R(const R &o) : v(o.v) { live++; }
    R(R &&o) noexcept : v(o.v) { live++; o.v = -1; }
    R &operator=(const R &o) { v = o.v; return *this; }
    R &operator=(R &&o) noexcept { v = o.v; o.v = -1; return *this; }
    ~R() { live--; }
    bool operator==(const R &o) const { return v == o.v; }
};

/* Its move constructor may throw, so a vector must COPY it when it grows
 * -- otherwise a throw halfway through relocation loses the elements
 * that were already moved out of the old buffer. */
struct Fragile {
    static int made, limit;
    int v;
    Fragile(int x = 0) : v(x) { if (made + 1 > limit) throw 1; ++made; }
    Fragile(const Fragile &o) : v(o.v)
    { if (made + 1 > limit) throw 1; ++made; }
    Fragile(Fragile &&o) : v(o.v)            /* NOT noexcept, on purpose */
    { if (made + 1 > limit) throw 1; ++made; o.v = -1; }
    ~Fragile() { --made; }
};
int Fragile::made, Fragile::limit = 100000;

int main()
{
    {
        vector<int> v;
        CHECK(v.empty() && v.size() == 0);
        for (int i = 0; i < 10; i++) v.push_back(i);
        CHECK(v.size() == 10 && v.front() == 0 && v.back() == 9);
        CHECK(v[4] == 4 && v.at(4) == 4);

        /* Doubling, so n appends cost O(n): capacity is a power of two
         * at least the size, never size + a constant. */
        CHECK(v.capacity() >= 10 && v.capacity() <= 16);

        int sum = 0;
        for (int x : v) sum += x;
        CHECK(sum == 45);
        /* Reverse iteration visits the same elements, backwards. */
        int first_rev = *v.rbegin(), last_rev = *(v.rend() - 1);
        CHECK(first_rev == 9 && last_rev == 0);
        CHECK(v.rend() - v.rbegin() == 10);

        v.pop_back();
        CHECK(v.size() == 9 && v.back() == 8);
        v.resize(3);
        CHECK(v.size() == 3 && v.back() == 2);
        v.resize(5, 7);
        CHECK(v.size() == 5 && v[3] == 7 && v[4] == 7);
        v.clear();
        CHECK(v.empty() && v.capacity() > 0);   /* clear keeps the storage */
    }

    /* Every element destroyed exactly once, however it got there. */
    {
        vector<R> v;
        for (int i = 0; i < 5; i++) v.emplace_back(i);
        CHECK(live == 5);
        CHECK(v[2].v == 2);
        v.erase(v.begin() + 1);
        CHECK(live == 4 && v.size() == 4 && v[1].v == 2);
        v.insert(v.begin(), R(99));
        CHECK(v[0].v == 99 && v.size() == 5);
        vector<R> w = move(v);
        CHECK(w.size() == 5 && v.empty());      /* moved-from is EMPTY */
        CHECK(live == 5);
    }
    CHECK(live == 0);

    /* Copy, compare, swap. */
    {
        vector<int> a{1, 2, 3}, b{1, 2, 3}, c{1, 2, 4};
        CHECK(a == b && a != c && a < c && c > a);
        vector<int> d = a;
        CHECK(d == a);
        d.swap(c);
        CHECK(d == vector<int>({1, 2, 4}) && c == a);
        CHECK(vector<int>({1, 2}) < vector<int>({1, 2, 0}));  /* prefix */
    }

    /* From a range and from an initializer_list. */
    {
        int raw[4] = {4, 5, 6, 7};
        vector<int> v(raw, raw + 4);
        CHECK(v.size() == 4 && v[3] == 7);
        vector<int> u{9, 8};
        CHECK(u.size() == 2 && u[0] == 9);
        vector<int> n(3, 5);
        CHECK(n.size() == 3 && n[2] == 5);
        vector<int> z(3);
        CHECK(z.size() == 3 && z[0] == 0);       /* value-initialised */
    }

    /* at() checks and operator[] does not. */
    {
        vector<int> v{1};
        int caught = 0;
        try { (void)v.at(5); } catch (const out_of_range &) { caught = 1; }
        CHECK(caught == 1);
    }

    /* A throwing move means the vector COPIES when it grows, so a throw
     * leaves it exactly as it was -- the strong guarantee. */
    {
        vector<Fragile> v;
        v.reserve(2);
        v.emplace_back(1);
        v.emplace_back(2);
        int before = Fragile::made;
        Fragile::limit = before + 1;    /* the second relocation throws */
        int caught = 0;
        try { v.emplace_back(3); } catch (int) { caught = 1; }
        Fragile::limit = 100000;
        CHECK(caught == 1);
        CHECK(v.size() == 2);           /* untouched */
        CHECK(v[0].v == 1 && v[1].v == 2);
        CHECK(Fragile::made == before); /* and nothing leaked */
    }

    /* erase_if, and that it really removes rather than shuffling. */
    {
        vector<int> v{1, 2, 3, 4, 5, 6};
        size_t n = erase_if(v, [](int x) { return x % 2 == 0; });
        CHECK(n == 3 && v.size() == 3);
        CHECK(v[0] == 1 && v[1] == 3 && v[2] == 5);
        n = erase(v, 3);
        CHECK(n == 1 && v.size() == 2 && v[1] == 5);
    }
    DONE();
}
