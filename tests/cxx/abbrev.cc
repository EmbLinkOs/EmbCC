// C++20's abbreviated function templates (9.3.4.6): `auto` and `C auto`
// parameters make a function a template of invented parameters — free
// functions, packs, constructors, members of classes and of class
// templates, beside declared template parameters; overloads ordered by
// the constraint. Also: a const T (&)[N] bound to a T[N], T(*)[] to
// const T(*)[] (span's compatibility test), and static_cast to a reference
// to an unrelated class through its conversion function.
// expect-exit: 42
#include <stdio.h>
template <class T> concept Small = sizeof(T) <= 4;
template <class T, class U> concept Same = __is_same(T, U);

static int twice(auto x) { return (int)x * 2; }
static int pick(Small auto x) { (void)x; return 1; }
static int pick(auto x) { (void)x; return 2; }
static int sum(auto... xs) { return (0 + ... + (int)xs); }
static int same_int(Same<int> auto x) { return x + 100; }
static int ref(const auto &x) { return (int)sizeof x; }
template <class T> static int mixed(T a, auto b) { return (int)sizeof(T) * 10 + (int)sizeof b; }

struct Box {
    int v;
    Box(Small auto x) : v((int)x) {}
    int add(auto y) const { return v + (int)y; }
    static int st(auto z) { return (int)z + 1; }
};
template <class T> struct Wrap {
    T t;
    template <class U> int both(U u, auto w) const { return (int)t + (int)u + (int)w; }
    int one(auto w) const { return (int)t * (int)w; }
};

// ---- arrays, and casts through a conversion ----
template <class T> struct id { typedef T type; };
template <unsigned long N> static int count(typename id<const int>::type (&a)[N])
{
    return (int)N * 100 + a[0];
}
template <class From, class To> constexpr bool conv = __is_convertible(From, To);
struct View { int x; };
struct Str { int y; operator View() const { return View{y}; } };
static int view_x(const Str &s) { return static_cast<const View &>(s).x; }

int main()
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s\n", #c); } } while (0)
    CHECK(twice(21) == 42 && twice(2.5) == 4);
    CHECK(pick('a') == 1 && pick(1L) == 2);
    CHECK(sum() == 0 && sum(1, 2, 3) == 6);
    CHECK(same_int(1) == 101);
    CHECK(ref(1.0) == 8 && ref('x') == 1);
    CHECK(mixed(1, 'c') == 41);
    Box b(7);
    CHECK(b.v == 7 && b.add(3) == 10 && Box::st(4) == 5);
    Wrap<int> w{3};
    CHECK(w.both(4L, 5) == 12 && w.one(6) == 18);
    int xs[3] = { 7, 8, 9 };
    CHECK(count(xs) == 307);
    CHECK((conv<int (*)[], const int (*)[]>) && !(conv<const int (*)[], int (*)[]>));
    CHECK(view_x(Str{5}) == 5);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
