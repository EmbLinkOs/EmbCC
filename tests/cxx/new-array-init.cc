// new T[n]{...}: the array is aggregate-initialized -- the leading
// elements from the list (with brace elision), the rest value-initialized;
// new T[]{...} takes its length from the list, new char[]{"..."} from the
// string; a class element's constructors run in order, and a throw part
// way destroys the ones built and frees the storage; a run-time bound
// under the list's length throws std::bad_array_new_length. (Before
// anything is allocated, as [expr.new] says -- g++ calls operator new[]
// first -- so the test asks only for the throw, which both do.)
// expect-exit: 42
#include <stdio.h>
#include <string.h>
static int fails;
static void check(const char *what, bool ok) { if (!ok) fails++; printf("%s %s\n", ok ? "ok  " : "FAIL", what); }

static char trail[128];
struct P {
    int v;
    P() : v(-1) { strcat(trail, "d"); }
    P(int x) : v(x) { if (x == 99) throw x; strcat(trail, "c"); }
    ~P() { strcat(trail, "~"); }
};
struct Pt { int x, y; };
static int allocs, frees;
void *operator new[](decltype(sizeof 0) n);
static unsigned char pool[4096];
static decltype(sizeof 0) used;
void *operator new[](decltype(sizeof 0) n)
{
    allocs++;
    void *p = pool + used;
    used += (n + 15) & ~(decltype(sizeof 0))15;
    return p;
}
void operator delete[](void *) noexcept { frees++; }
void operator delete[](void *, decltype(sizeof 0)) noexcept { frees++; }

static int run_count(int n) { return n; }

int main()
{
    int *a = new int[5]{1, 2, 3};
    check("constant bound: the list, then zeros",
          a[0] == 1 && a[1] == 2 && a[2] == 3 && a[3] == 0 && a[4] == 0);
    int n = run_count(6);
    long *b = new long[n]{7, 8};
    check("run-time bound", b[0] == 7 && b[1] == 8 && b[2] == 0 && b[5] == 0);
    int *c = new int[]{4, 5, 6, 7};
    check("new int[]: the list's length", c[0] == 4 && c[3] == 7);
    char *s = new char[]{"hi!"};
    check("new char[]{\"hi!\"}", strcmp(s, "hi!") == 0 && s[3] == 0);
    Pt *pts = new Pt[3]{{1, 2}, {3, 4}};
    check("aggregates, the last value-initialized",
          pts[0].x == 1 && pts[1].y == 4 && pts[2].x == 0 && pts[2].y == 0);
    int (*m)[2] = new int[2][2]{1, 2, 3};
    check("brace elision across rows",
          m[0][0] == 1 && m[0][1] == 2 && m[1][0] == 3 && m[1][1] == 0);
    double *d = new double[3](1.5, 2.5);
    check("C++20 parenthesized", d[0] == 1.5 && d[1] == 2.5 && d[2] == 0);

    trail[0] = 0;
    P *p = new P[4]{10, 20};
    check("constructors in order: two from the list, two defaults",
          strcmp(trail, "ccdd") == 0 && p[0].v == 10 && p[1].v == 20 &&
          p[2].v == -1 && p[3].v == -1);
    trail[0] = 0;
    delete[] p;
    check("delete[] destroys all four", strcmp(trail, "~~~~") == 0);

    trail[0] = 0;
    int caught = 0, frees0 = frees;
    try {
        P *q = new P[3]{1, 99, 3};
        (void)q;
    } catch (int x) {
        caught = x;
    }
    check("a throw part way destroys what was built",
          caught == 99 && strcmp(trail, "c~") == 0);
    check("... and frees the storage", frees == frees0 + 1);

    int small = run_count(2);
    caught = 0;
    try {
        int *r = new int[small]{1, 2, 3};
        (void)r;
    } catch (...) {
        caught = 1;
    }
    check("bound under the list: thrown", caught == 1);
    return fails == 0 ? 42 : 1;
}
