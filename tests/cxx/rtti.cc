// CX3: RTTI — typeid of types and of polymorphic objects (their dynamic
// type, through the vtable), and dynamic_cast down and across a class
// hierarchy, to void *, and failing — all through libsupc++'s runtime and
// the typeinfo objects EmbCC emits in Itanium's layout.
// expect-exit: 42
#include <stdio.h>
#include <string.h>

// std::type_info as libstdc++'s <typeinfo> declares it (that header comes
// with libstdc++ itself, CX8): its layout is the ABI's
namespace std {
class type_info {
public:
    virtual ~type_info();
    const char *name() const { return __name[0] == '*' ? __name + 1 : __name; }
    bool operator==(const type_info &o) const
    {
        return __name == o.__name ||
               (__name[0] != '*' && strcmp(__name, o.name()) == 0);
    }
    bool operator!=(const type_info &o) const { return !(*this == o); }
protected:
    const char *__name;
};
}

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

struct Animal {
    virtual ~Animal() {}
    virtual const char *sound() const { return "..."; }
};
struct Dog : Animal {
    const char *sound() const override { return "woof"; }
};
struct Puppy : Dog {};
struct Cat : Animal {};

struct Swimmer {
    virtual ~Swimmer() {}
    int depth = 5;
};
struct Duck : Animal, Swimmer {};

struct Plain {
    int x;
};

int main()
{
    Puppy p;
    Animal *a = &p;
    check("typeid of the dynamic type", typeid(*a) == typeid(Puppy) &&
                                        strcmp(typeid(*a).name(), "5Puppy") == 0);
    check("typeid of types", typeid(int) == typeid(int) &&
                             typeid(Dog) != typeid(Cat) &&
                             strcmp(typeid(Plain).name(), "5Plain") == 0);
    check("typeid of a non-polymorphic lvalue",
          strcmp(typeid(p).name(), "5Puppy") == 0);

    check("dynamic_cast down", dynamic_cast<Dog *>(a) == &p &&
                               dynamic_cast<Puppy *>(a) == &p);
    check("dynamic_cast failing", dynamic_cast<Cat *>(a) == nullptr);
    Animal *none = nullptr;
    check("dynamic_cast of null", dynamic_cast<Dog *>(none) == nullptr);
    check("dynamic_cast to void *", dynamic_cast<void *>(a) == (void *)&p);

    Duck d;
    Swimmer *s = &d;
    Animal *da = dynamic_cast<Animal *>(s);
    check("dynamic_cast across bases", da == static_cast<Animal *>(&d) &&
                                       dynamic_cast<Duck *>(s) == &d);
    check("dynamic_cast to void * from a secondary base",
          dynamic_cast<void *>(s) == (void *)&d);
    Dog &dr = dynamic_cast<Dog &>(*a);
    check("dynamic_cast of a reference", strcmp(dr.sound(), "woof") == 0);
    check("upcast is static", dynamic_cast<Animal *>(&p) == a);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
