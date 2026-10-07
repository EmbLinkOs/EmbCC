// RTTI without exceptions: typeid, type_info's names and comparisons,
// and dynamic_cast down, across and to void through single, multiple and
// virtual inheritance -- what the type_info objects EmbCC emits and the
// embedded runtime's __dynamic_cast must agree on (the vmi type_info's
// flags and base count are two words where a pointer is four bytes).
// Built WITH RTTI (embedded-flags below); a failed cast is to a pointer,
// since the reference form would have to throw. Each line printed is one
// property; the host's clang++ build prints the same.
// embedded-flags: -frtti
#include <stdio.h>
#include <string.h>
#include <typeinfo>

struct Base { virtual ~Base() {} int b = 1; };
struct Mid : Base { int m = 2; };
struct Leaf : Mid { int l = 3; };
struct Other { virtual ~Other() {} int o = 4; };
struct Multi : Mid, Other { int x = 5; };
struct V { virtual ~V() {} int v = 6; };
struct VL : virtual V { int vl = 7; };
struct VR : virtual V { int vr = 8; };
struct VD : VL, VR { int vd = 9; };
struct Plain { int p; };

const char *kind(Base *p)
{
    if (dynamic_cast<Leaf *>(p))
        return "leaf";
    if (dynamic_cast<Multi *>(p))
        return "multi";
    if (dynamic_cast<Mid *>(p))
        return "mid";
    return "base";
}

int main()
{
    Base b;
    Mid m;
    Leaf l;
    Multi mu;
    Base *all[4] = { &b, &m, &l, &mu };
    for (int i = 0; i < 4; i++)
        printf("%s %s\n", kind(all[i]), typeid(*all[i]).name());

    Base &rb = l;
    printf("typeid same %d %d %d\n", typeid(rb) == typeid(Leaf),
           typeid(rb) == typeid(Base), typeid(Leaf) != typeid(Mid));
    printf("names %s %s %s %s\n", typeid(int).name(), typeid(Plain).name(),
           typeid(Base *).name(), typeid(const char *).name());

    // across: from one base of Multi to the other
    Base *mb = &mu;
    Other *mo = dynamic_cast<Other *>(mb);
    printf("cross %d %d same %d\n", mo != nullptr, mo ? mo->o : -1,
           mo == static_cast<Other *>(&mu));
    Other *lo = dynamic_cast<Other *>(static_cast<Base *>(&l));
    printf("cross fails %d\n", lo == nullptr);
    void *whole = dynamic_cast<void *>(mo);
    printf("to void %d\n", whole == static_cast<void *>(&mu));

    // through virtual bases
    VD vd;
    V *pv = &vd;
    VR *pr = dynamic_cast<VR *>(pv);
    VL *pl = dynamic_cast<VL *>(pr);
    VD *pd = dynamic_cast<VD *>(pv);
    printf("virtual %d %d %d %d\n", pr ? pr->vr : -1, pl ? pl->vl : -1,
           pd ? pd->vd : -1, pd == &vd);
    printf("virtual names %s %s\n", typeid(*pv).name(), typeid(VL).name());
    const std::type_info &ti = typeid(*pv);
    printf("before %d hash eq %d\n", ti.before(ti),
           ti.hash_code() == typeid(VD).hash_code());
    printf("==END==\n");
    fflush(stdout);
    return 0;
}
