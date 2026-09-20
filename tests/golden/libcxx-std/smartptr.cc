/* shared_ptr, weak_ptr, and the unordered containers. */
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include "check.h"
using namespace std;

static int live, dtors;
struct R {
    int v;
    R(int x = 0) : v(x) { live++; }
    ~R() { live--; dtors++; }
};
struct Base { virtual ~Base() {} };
struct Derived : Base { int v = 7; };

struct Node : enable_shared_from_this<Node> {
    int v;
    Node(int x) : v(x) {}
    shared_ptr<Node> self() { return shared_from_this(); }
};

int main()
{
    /* ---- shared_ptr ---- */
    {
        auto p = make_shared<R>(5);
        CHECK(p->v == 5 && live == 1 && p.use_count() == 1);
        {
            auto q = p;
            CHECK(p.use_count() == 2 && q.use_count() == 2);
            CHECK(live == 1);            /* shared, not copied */
        }
        CHECK(p.use_count() == 1 && live == 1);
        p.reset();
        CHECK(live == 0 && dtors == 1);
    }
    {
        shared_ptr<R> a(new R(1));
        CHECK(live == 1);
        shared_ptr<R> b = std::move(a);
        CHECK(!a && b && b.use_count() == 1 && live == 1);
    }
    CHECK(live == 0);

    /* Self-assignment must not destroy what it keeps: the new reference
     * is taken before the old one is released. */
    {
        auto p = make_shared<R>(3);
        p = p;
        CHECK(p && p->v == 3 && live == 1);
        auto &ref = p;
        p = ref;
        CHECK(p && p->v == 3 && live == 1);
    }
    CHECK(live == 0);

    /* ---- weak_ptr: two counts, and why ---- */
    {
        weak_ptr<R> w;
        {
            auto p = make_shared<R>(9);
            w = p;
            CHECK(!w.expired() && w.use_count() == 1);
            auto locked = w.lock();
            CHECK(locked && locked->v == 9);
            CHECK(p.use_count() == 2);   /* lock() took a strong ref */
        }
        /* The object is gone but the control block is not -- which is
         * how expired() can be asked at all. */
        CHECK(w.expired());
        CHECK(live == 0);
        CHECK(!w.lock());                /* and lock() reports it safely */
    }

    /* A weak_ptr does not keep the object alive: the classic cycle. */
    {
        struct Cyc { shared_ptr<Cyc> strong; weak_ptr<Cyc> weak; int v = 0; };
        {
            auto a = make_shared<Cyc>();
            auto b = make_shared<Cyc>();
            a->weak = b;                 /* weak, so no cycle */
            b->weak = a;
            CHECK(a.use_count() == 1 && b.use_count() == 1);
        }
        /* With `strong` on both sides they would leak; with `weak` they
         * do not, and there is nothing left to check but that we got
         * here. */
    }

    /* enable_shared_from_this: the object learns its control block from
     * the shared_ptr that first owned it. */
    {
        auto n = make_shared<Node>(4);
        auto again = n->self();
        CHECK(again && again->v == 4);
        CHECK(n.use_count() == 2);
    }

    /* Casts share ownership. */
    {
        shared_ptr<Base> b = make_shared<Derived>();
        auto d = dynamic_pointer_cast<Derived>(b);
        CHECK(d && d->v == 7);
        CHECK(b.use_count() == 2);       /* they share one block */
        shared_ptr<Base> none = make_shared<Base>();
        CHECK(!dynamic_pointer_cast<Derived>(none));
    }

    /* A custom deleter runs instead of delete. */
    {
        int deleted = 0;
        {
            int x = 0;
            shared_ptr<int> p(&x, [&](int *) { deleted = 1; });
            CHECK(p.get() == &x);
        }
        CHECK(deleted == 1);
    }

    /* ---- unordered_map ---- */
    {
        unordered_map<string, int> m;
        m["one"] = 1;
        m["two"] = 2;
        CHECK(m.size() == 2 && m["one"] == 1);
        CHECK(m.count("one") == 1 && m.count("zzz") == 0);
        CHECK(m.contains("two"));
        CHECK(m.find("zzz") == m.end());
        m["three"] = 3;
        CHECK(m.at("three") == 3);
        CHECK(m.erase("one") == 1 && m.erase("one") == 0);
        CHECK(m.size() == 2);
        CHECK(m.__check());
    }

    /* Growth: sequential integer keys are the input that piles every
     * element into one bucket when the hash is the identity and no
     * mixing is applied. Every key must still be findable after the
     * rehashes, and the table's own invariants must hold. */
    {
        unordered_map<int, int> m;
        for (int i = 0; i < 2000; i++) m[i] = i * 3;
        CHECK(m.size() == 2000);
        CHECK(m.__check());
        for (int i = 0; i < 2000; i++) CHECK(m[i] == i * 3);
        CHECK(m.load_factor() <= 1.0f);
        CHECK(m.bucket_count() >= 2000);

        /* Iteration visits every element exactly once, whatever order. */
        int n = 0; long sum = 0;
        for (auto &kv : m) { n++; sum += kv.first; }
        CHECK(n == 2000);
        CHECK(sum == 1999L * 2000 / 2);

        for (int i = 0; i < 2000; i += 2) m.erase(i);
        CHECK(m.size() == 1000 && m.__check());
        CHECK(m.count(1) == 1 && m.count(0) == 0);
    }

    /* A reference to an element survives a rehash, because the nodes are
     * relinked rather than reallocated. */
    {
        unordered_map<int, string> m;
        m[1] = "kept";
        string &r = m[1];
        for (int i = 2; i < 500; i++) m[i] = "x";
        CHECK(r == "kept");
        CHECK(m[1] == "kept");
    }

    /* ---- unordered_set ---- */
    {
        unordered_set<int> s{1, 2, 3, 2, 1};
        CHECK(s.size() == 3);
        CHECK(s.contains(2) && !s.contains(9));
        CHECK(!s.insert(2).second);
        CHECK(s.insert(9).second);
        CHECK(s.erase(9) == 1);
        CHECK(s.__check());

        unordered_set<string> ss{"a", "b"};
        CHECK(ss.contains("a") && ss.size() == 2);
    }

    /* Multi keeps duplicates. */
    {
        unordered_multiset<int> ms;
        for (int i = 0; i < 5; i++) ms.insert(7);
        CHECK(ms.size() == 5 && ms.count(7) == 5);
        CHECK(ms.erase(7) == 5 && ms.size() == 0);
    }

    /* +0.0 and -0.0 compare equal, so they must hash equally. */
    {
        unordered_set<double> s;
        s.insert(0.0);
        CHECK(s.contains(-0.0));
        CHECK(s.size() == 1);
        s.insert(-0.0);
        CHECK(s.size() == 1);
    }
    DONE();
}
