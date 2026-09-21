/* <map> and <set>: the red-black tree, and the invariants that make its
 * complexity guarantee true rather than merely usual. */
#include <map>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
#include "check.h"
using namespace std;

int main()
{
    /* ---- map ---- */
    {
        map<string, int> m;
        CHECK(m.empty() && m.size() == 0);
        m["one"] = 1;
        m["two"] = 2;
        m["three"] = 3;
        CHECK(m.size() == 3);
        CHECK(m["two"] == 2);
        CHECK(m.at("one") == 1);

        /* Iteration is IN KEY ORDER -- the reason to choose a map. */
        vector<string> keys;
        for (auto &kv : m) keys.push_back(kv.first);
        CHECK(keys.size() == 3);
        CHECK(keys[0] == "one" && keys[1] == "three" && keys[2] == "two");

        /* operator[] INSERTS when absent, which is why it has no const
         * overload and why find() exists. */
        CHECK(m.count("four") == 0);
        CHECK(m["four"] == 0);           /* default-constructed, and added */
        CHECK(m.size() == 4);
        CHECK(m.count("four") == 1);

        CHECK(m.find("one") != m.end());
        CHECK(m.find("zzz") == m.end());
        CHECK(m.contains("two"));

        int caught = 0;
        try { (void)m.at("nope"); } catch (const out_of_range &) { caught = 1; }
        CHECK(caught == 1);

        /* insert does not overwrite; insert_or_assign does. */
        auto r = m.insert({"one", 99});
        CHECK(!r.second && m["one"] == 1);
        m.insert_or_assign("one", 99);
        CHECK(m["one"] == 99);

        /* try_emplace does not construct the value when the key is
         * present -- the difference from emplace. */
        auto t = m.try_emplace("one", 5);
        CHECK(!t.second && m["one"] == 99);

        CHECK(m.erase("four") == 1);
        CHECK(m.erase("four") == 0);
        CHECK(m.size() == 3);
        CHECK(m.__check());
    }

    /* ---- the tree's invariants, at a size where they matter ---- */
    {
        /* Ascending insertion is the input that turns an unbalanced
         * binary search tree into a linked list. The red-black
         * invariants are checked directly, because a tree that is merely
         * "sorted and works" can be arbitrarily deep and pass every
         * functional test. */
        map<int, int> m;
        for (int i = 0; i < 1000; i++) {
            m[i] = i * 2;
            if (i % 97 == 0) CHECK(m.__check());
        }
        CHECK(m.size() == 1000);
        CHECK(m.__check());
        CHECK(m[500] == 1000);

        /* Descending, and interleaved, and then erased back to empty. */
        map<int, int> d;
        for (int i = 1000; i > 0; i--) d[i] = i;
        CHECK(d.__check() && d.size() == 1000);
        map<int, int> x;
        for (int i = 0; i < 500; i++) { x[i] = i; x[1000 - i] = i; }
        CHECK(x.__check());
        for (int i = 0; i < 1000; i++) {
            m.erase(i);
            if (i % 89 == 0) CHECK(m.__check());
        }
        CHECK(m.empty() && m.__check());

        /* Erasing a node with two children relinks rather than copies, so
         * an iterator to the successor stays valid. */
        map<int, int> e;
        for (int i = 0; i < 20; i++) e[i] = i;
        auto it = e.find(11);
        e.erase(10);
        CHECK(it->first == 11 && it->second == 11);
        CHECK(e.__check());
    }

    /* Erasing while iterating: erase returns the next iterator, which is
     * the only way to write this loop correctly. */
    {
        map<int, int> m;
        for (int i = 0; i < 10; i++) m[i] = i;
        for (auto it = m.begin(); it != m.end(); ) {
            if (it->first % 2 == 0) it = m.erase(it);
            else ++it;
        }
        CHECK(m.size() == 5);
        CHECK(m.count(1) == 1 && m.count(2) == 0);
        CHECK(m.__check());
    }

    /* ---- ordered queries ---- */
    {
        map<int, int> m;
        for (int i = 0; i < 10; i++) m[i * 10] = i;
        CHECK(m.lower_bound(30)->first == 30);
        CHECK(m.upper_bound(30)->first == 40);
        CHECK(m.lower_bound(35)->first == 40);
        CHECK(m.lower_bound(1000) == m.end());
    }

    /* ---- set ---- */
    {
        set<int> s{5, 3, 9, 1, 3};
        CHECK(s.size() == 4);            /* the duplicate 3 was rejected */
        vector<int> v(s.begin(), s.end());
        CHECK(v[0] == 1 && v[1] == 3 && v[2] == 5 && v[3] == 9);
        CHECK(s.contains(5) && !s.contains(4));
        CHECK(s.insert(5).second == false);
        CHECK(s.insert(7).second == true);
        CHECK(s.erase(9) == 1 && s.erase(9) == 0);
        CHECK(s.__check());

        set<string> ss;
        for (const char *w : {"pear", "apple", "fig"}) ss.insert(w);
        CHECK(*ss.begin() == "apple");
        CHECK(ss.size() == 3);
    }

    /* ---- multimap and multiset keep equal keys ---- */
    {
        multimap<int, char> mm;
        mm.insert({1, 'a'});
        mm.insert({1, 'b'});
        mm.insert({2, 'c'});
        CHECK(mm.size() == 3);
        CHECK(mm.count(1) == 2);
        auto r = mm.equal_range(1);
        int n = 0;
        for (auto it = r.first; it != r.second; ++it) n++;
        CHECK(n == 2);
        CHECK(mm.erase(1) == 2);
        CHECK(mm.size() == 1);
        CHECK(mm.__check());

        multiset<int> ms{1, 1, 2, 3, 3, 3};
        CHECK(ms.size() == 6);
        CHECK(ms.count(3) == 3);
        CHECK(ms.__check());
    }

    /* ---- copy, move, compare ---- */
    {
        map<int, int> a;
        for (int i = 0; i < 50; i++) a[i] = i;
        map<int, int> b = a;
        CHECK(b == a && b.__check());
        b[100] = 100;
        CHECK(b != a);
        map<int, int> c = std::move(b);
        CHECK(c.size() == 51 && b.empty());
        c.swap(a);
        CHECK(a.size() == 51 && c.size() == 50);
    }
    DONE();
}
