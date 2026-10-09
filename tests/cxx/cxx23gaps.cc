// Five pieces of C++17-23 the front end refused (docs/manual/cxx.md):
// switch with an init-statement, a range-based for with one, u8 character
// literals, `namespace a::inline b`, and auto(x) / auto{x}. Each is checked
// for what it MEANS, not only that it parses: the init runs once and is
// in scope; the literal's value and type; the nested namespace is inline
// (its names are found from the enclosing one); auto(x) is a copy.
// expect-exit: 42
static int fails;
#define CHECK(c) do { if (!(c)) fails++; } while (0)

static int calls;
static int next_val() { return ++calls; }

static int sw(int x)
{
    switch (int y = x * 2; y) {           // init, then the condition
    case 4: return y + 100;
    default: return y;
    }
}

static int sw_decl(int x)
{
    switch (int k = next_val(); int y = x + k) {   // init AND a condition decl
    case 3: return 30;
    default: return y;
    }
}

struct Counter { int n; };
static int range_init()
{
    int a[3] = { 1, 2, 3 };
    int s = 0;
    for (Counter c{10}; int v : a)        // c is made once, before the loop
        s += v + c.n++;
    return s;                             // (1+10) + (2+11) + (3+12)
}

namespace outer::inline inner { int value = 7; int f() { return 8; } }

struct Big { int a[4]; };
static int mutate(Big b) { b.a[0] = 99; return b.a[0]; }

int main()
{
    CHECK(sw(2) == 104);
    CHECK(sw(5) == 10);
    calls = 0;
    CHECK(sw_decl(2) == 30);              // k = 1, y = 3
    CHECK(calls == 1);
    CHECK(range_init() == 39);

    auto c = u8'a';
    CHECK(c == 97);
    CHECK(sizeof(u8'z') == 1);
    char8_t c8 = u8'\x7f';
    CHECK(c8 == 127);

    CHECK(outer::value == 7);             // found through the inline namespace
    CHECK(outer::inner::value == 7);
    CHECK(outer::f() == 8);

    int x = 5;
    auto y = auto(x);
    y += 1;
    CHECK(x == 5 && y == 6);              // a copy
    const int cx = 3;
    auto z = auto{cx};
    z = 4;                                // decayed: not const
    CHECK(z == 4);
    Big b{{1, 2, 3, 4}};
    CHECK(mutate(auto(b)) == 99 && b.a[0] == 1);
    int arr[2] = { 9, 8 };
    auto p = auto(arr);                   // an array decays to a pointer
    CHECK(p[1] == 8);

    return fails ? fails : 42;
}
