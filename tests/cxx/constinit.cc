// Constant initialization (6.9.3.2): a static object whose initializer is
// a constant expression — a constexpr function's call included — has its
// value before any dynamic initializer runs, as g++ gives it. Here a
// constructor that runs earlier in the unit's order reads variables
// defined after it: they must hold their values already. Also a local
// static (no guard needed), `constinit`, an aggregate with such members,
// and the initializers that stay dynamic (a non-constexpr call, a read of
// a variable that is not a constant) — run in the order of their
// definitions (6.9.3.3), not of earlier extern declarations.
// expect-exit: 42
#include <stdio.h>

extern const long early;
extern long plain;
extern const unsigned long long ubig;
struct Pair { int a; long b; };
extern Pair pair;
extern int dyn_value;
extern int second, first;                 // declared in the other order

struct Reader {
    long seen_early, seen_plain, seen_pair;
    unsigned long long seen_ubig;
    Reader()
        : seen_early(early), seen_plain(plain), seen_pair(pair.a + pair.b),
          seen_ubig(ubig) {}
};
Reader reader;                            // dynamic: runs in order

constexpr long sq(long x) { return x * x; }
constexpr unsigned long long top(int n)
{
    unsigned long long r = 0;
    for (int i = 0; i < n; i++)
        r = r << 1 | 1;
    return r << (64 - n);
}
int runtime(int x) { return x + 1; }      // not constexpr

const long early = sq(7);
long plain = sq(1L << 20) + 3;
const unsigned long long ubig = top(3);   // 0xe000000000000000
Pair pair = { (int)sq(4), sq(5) };
constinit long ci = sq(6);
int dyn_value = runtime(41);              // dynamic
int copy = dyn_value;                     // dynamic: not a constant
int first = runtime(1);                   // 2
int second = first * 10 + runtime(0);     // after first: 21

int counter()
{
    static long n = sq(3);                // constant: no guard
    return (int)n++;
}

int main()
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s\n", #c); } } while (0)
    CHECK(reader.seen_early == 49);
    CHECK(reader.seen_plain == (1L << 40) + 3);
    CHECK(reader.seen_pair == 16 + 25);
    CHECK(reader.seen_ubig == 0xe000000000000000ULL);
    CHECK(ci == 36 && dyn_value == 42 && copy == 42);
    CHECK(first == 2 && second == 21);
    CHECK(counter() == 9 && counter() == 10);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
