/* <memory>: where raw storage becomes objects, and where ownership is
 * written down. */
#include <memory>
#include "check.h"
using namespace std;

static int live, dtors;
struct R { int v; R(int x = 0) : v(x) { live++; } ~R() { live--; dtors++; } };

/* Overloads unary &, so `&w` is null and addressof still is not -- which
 * is the entire reason addressof exists. */
struct Weird { int v; Weird *operator&() { return nullptr; } };

/* Counted on SUCCESS only, so `made` is the number of LIVE objects and a
 * rollback failure shows as a leak rather than as the throwing
 * constructor's own increment. */
struct Throwing {
    static int made, limit;
    int v;
    Throwing(int x = 0) : v(x) { if (made + 1 > limit) throw 1; ++made; }
    Throwing(const Throwing &o) : v(o.v)
    { if (made + 1 > limit) throw 1; ++made; }
    ~Throwing() { --made; }
};
int Throwing::made, Throwing::limit = 1000;

int main()
{
    {
        auto p = make_unique<R>(7);
        CHECK(p->v == 7);
        CHECK(live == 1);
        auto q = move(p);
        CHECK(q->v == 7);
        CHECK(!p);                    /* moved from, and says so */
        CHECK(live == 1);             /* moving did not construct anything */
        q.reset();
        CHECK(live == 0 && dtors == 1);
    }
    { auto a = make_unique<R[]>(3); CHECK(live == 3); }
    CHECK(live == 0);

    /* Self-move must not destroy the object: a reset() before the release
     * would free it and then store the pointer it had just freed. */
    auto s = make_unique<R>(5);
    R *raw = s.get();
    s = move(s);
    CHECK(s.get() == raw && live == 1);
    s.reset();

    Weird w{5};
    CHECK(addressof(w) != nullptr);
    CHECK(&w == nullptr);

    /* uninitialized_copy must destroy what it built when one throws. */
    alignas(Throwing) unsigned char buf[sizeof(Throwing) * 8];
    Throwing src[4] = {Throwing(1), Throwing(2), Throwing(3), Throwing(4)};
    int before = Throwing::made;
    Throwing::limit = before + 2;             /* the third copy throws */
    int caught = 0;
    try {
        uninitialized_copy(src, src + 4, reinterpret_cast<Throwing *>(buf));
    } catch (int) { caught = 1; }
    Throwing::limit = 1000;
    CHECK(caught == 1);
    CHECK(Throwing::made == before);          /* nothing leaked */

    /* construct_at / destroy_at on raw storage. */
    alignas(R) unsigned char rb[sizeof(R)];
    R *rp = construct_at(reinterpret_cast<R *>(rb), 11);
    CHECK(rp->v == 11 && live == 1);
    destroy_at(rp);
    CHECK(live == 0);

    /* allocate() must refuse a count that would wrap when scaled. */
    allocator<double> al;
    int threw = 0;
    try { (void)al.allocate((size_t)-1 / 4); }
    catch (const bad_array_new_length &) { threw = 1; }
    catch (const bad_alloc &) { threw = 1; }
    CHECK(threw == 1);
    double *d = al.allocate(4);
    CHECK(d != nullptr);
    al.deallocate(d, 4);
    DONE();
}
