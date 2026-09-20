/* <list>, <deque>, <forward_list>, and the adaptors over them. */
#include <list>
#include <deque>
#include <forward_list>
#include <queue>
#include <stack>
#include <string>
#include <vector>
#include <algorithm>
#include "check.h"
using namespace std;

static int live;
struct R { int v; R(int x = 0) : v(x) { live++; }
           R(const R &o) : v(o.v) { live++; }
           R(R &&o) noexcept : v(o.v) { live++; o.v = -1; }
           ~R() { live--; }
           bool operator==(const R &o) const { return v == o.v; } };

int main()
{
    /* ---- list ---- */
    {
        list<int> l{1, 2, 3};
        CHECK(l.size() == 3 && l.front() == 1 && l.back() == 3);
        l.push_front(0);
        l.push_back(4);
        CHECK(l.size() == 5 && l.front() == 0 && l.back() == 4);

        vector<int> seen(l.begin(), l.end());
        CHECK(seen.size() == 5 && seen[0] == 0 && seen[4] == 4);

        /* Backwards, which a forward_list cannot do. */
        vector<int> rev;
        for (auto it = l.end(); it != l.begin(); ) rev.push_back(*--it);
        CHECK(rev[0] == 4 && rev[4] == 0);

        l.pop_front(); l.pop_back();
        CHECK(l.size() == 3 && l.front() == 1);

        /* Erasing invalidates ONLY the erased element's iterator -- the
         * reason to choose a list. */
        auto keep = l.begin(); ++keep;          /* the 2 */
        l.erase(l.begin());
        CHECK(*keep == 2 && l.size() == 2);

        l.remove(2);
        CHECK(l.size() == 1 && l.front() == 3);
        l.reverse();
        CHECK(l.front() == 3);
    }

    /* splice MOVES nodes: no copy, no allocation, and iterators into the
     * moved range stay valid. */
    {
        list<int> a{1, 2}, b{3, 4};
        auto it = b.begin();
        a.splice(a.end(), b);
        CHECK(a.size() == 4 && b.empty());
        CHECK(*it == 3);                        /* still valid, now in a */
        vector<int> v(a.begin(), a.end());
        CHECK(v[0] == 1 && v[2] == 3 && v[3] == 4);
    }

    /* Every element destroyed exactly once, through every path. */
    {
        {
            list<R> l;
            for (int i = 0; i < 5; i++) l.emplace_back(i);
            CHECK(live == 5);
            list<R> c = l;
            CHECK(live == 10);
            list<R> m = std::move(c);
            CHECK(live == 10 && c.empty() && m.size() == 5);
        }
        CHECK(live == 0);
    }

    /* Moving re-closes the ring around THIS object's sentinel: the
     * sentinel is a member and does not travel with the nodes. */
    {
        list<int> a{1, 2, 3};
        list<int> b = std::move(a);
        CHECK(b.size() == 3 && a.empty());
        b.push_back(4);
        CHECK(b.back() == 4);
        a.push_back(9);                          /* the moved-from is usable */
        CHECK(a.size() == 1 && a.front() == 9);
        vector<int> v(b.begin(), b.end());
        CHECK(v.size() == 4 && v[3] == 4);
    }

    /* ---- deque ---- */
    {
        deque<int> d;
        for (int i = 0; i < 5; i++) d.push_back(i);
        for (int i = 1; i <= 5; i++) d.push_front(-i);
        CHECK(d.size() == 10);
        CHECK(d.front() == -5 && d.back() == 4);
        CHECK(d[0] == -5 && d[5] == 0 && d[9] == 4);
        CHECK(d.at(4) == -1);

        int caught = 0;
        try { (void)d.at(99); } catch (const out_of_range &) { caught = 1; }
        CHECK(caught == 1);

        d.pop_front(); d.pop_back();
        CHECK(d.size() == 8 && d.front() == -4 && d.back() == 3);

        /* Random access, which a list does not have. */
        CHECK(d.end() - d.begin() == 8);
        CHECK(*(d.begin() + 3) == -1);
        CHECK(is_sorted(d.begin(), d.end()));
    }

    /* Wrapping: pushing at both ends across the ring's seam, many
     * times, so the mask arithmetic is exercised rather than assumed. */
    {
        deque<int> d;
        for (int round = 0; round < 200; round++) {
            d.push_back(round);
            d.push_front(-round);
            if (round % 3 == 0 && d.size() >= 2) { d.pop_front(); d.pop_back(); }
        }
        int n = (int)d.size();
        CHECK(n > 0);
        /* Read every element through both paths and agree. */
        int i = 0;
        for (auto it = d.begin(); it != d.end(); ++it, ++i)
            CHECK(*it == d[i]);
        CHECK(i == n);
    }
    {
        deque<R> d;
        for (int i = 0; i < 100; i++) d.push_front(R(i));
        CHECK(live == 100);
        d.clear();
        CHECK(live == 0);
    }

    /* ---- forward_list ---- */
    {
        forward_list<int> f{1, 2, 3};
        CHECK(f.front() == 1);
        vector<int> v(f.begin(), f.end());
        CHECK(v.size() == 3 && v[0] == 1 && v[2] == 3);

        f.push_front(0);
        CHECK(f.front() == 0);

        /* insert_after and erase_after, because a singly-linked node
         * cannot reach its predecessor. */
        auto it = f.begin();                     /* the 0 */
        f.insert_after(it, 99);
        vector<int> w(f.begin(), f.end());
        CHECK(w[0] == 0 && w[1] == 99 && w[2] == 1);
        f.erase_after(it);
        vector<int> x(f.begin(), f.end());
        CHECK(x[1] == 1);

        /* before_begin names the position before the first element, so
         * insert_after has something to name for "at the front". */
        f.insert_after(f.before_begin(), 7);
        CHECK(f.front() == 7);

        f.reverse();
        CHECK(f.front() == 3);
    }

    /* ---- adaptors ---- */
    {
        queue<int> q;
        for (int i = 0; i < 5; i++) q.push(i);
        CHECK(q.size() == 5 && q.front() == 0 && q.back() == 4);
        q.pop();
        CHECK(q.front() == 1 && q.size() == 4);
        while (!q.empty()) q.pop();
        CHECK(q.empty());
    }
    {
        stack<string> s;
        s.push("a"); s.push("b");
        CHECK(s.top() == "b" && s.size() == 2);
        s.pop();
        CHECK(s.top() == "a");
    }
    {
        /* less<> gives a MAX-heap: top() is the greatest. Wanting the
         * smallest means greater<>, which reads backwards. */
        priority_queue<int> pq;
        for (int v : {3, 1, 4, 1, 5, 9, 2, 6}) pq.push(v);
        CHECK(pq.size() == 8 && pq.top() == 9);
        vector<int> out;
        while (!pq.empty()) { out.push_back(pq.top()); pq.pop(); }
        CHECK(out.size() == 8);
        CHECK(is_sorted(out.rbegin(), out.rend()));   /* descending */
        CHECK(out[0] == 9 && out[7] == 1);

        priority_queue<int, vector<int>, greater<int>> mn;
        for (int v : {3, 1, 4}) mn.push(v);
        CHECK(mn.top() == 1);
    }
    DONE();
}
