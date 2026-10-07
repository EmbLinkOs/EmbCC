// Static storage: namespace-scope objects with constructors (run from
// .init_array before main, in definition order), function-local statics
// (guarded: initialized once, on first use), and their destructors
// (registered at construction, run by exit in the reverse order). Each line
// printed is one property; the host's clang++ build prints the same.
#include <stdio.h>
#include <stdlib.h>

static int order;

struct Tracer {
    const char *name;
    int seq;
    explicit Tracer(const char *n) : name(n), seq(++order) {
        printf("construct %s #%d\n", name, seq);
    }
    ~Tracer() {
        printf("destroy %s #%d\n", name, seq);
        // the first object built is the last destroyed: the end of the run
        if (seq == 1) {
            printf("==END==\n");
            fflush(stdout);
        }
    }
};

Tracer first("first");          // dynamic: a constructor runs before main

int compute(int x) { return x * 3 + order; }
int dyn_int = compute(4);        // dynamic initialization of a scalar

struct Plain { int a, b; };
Plain plain = { 5, 6 };          // constant: no code at all

struct Config {
    int baud;
    constexpr Config(int b) : baud(b) {}
};
constexpr Config cfg(115200);    // constexpr constructor: constant

namespace drivers {
Tracer second("second");
}

int calls;

Tracer &instance()
{
    static Tracer t("singleton");     // guarded, built on the first call
    calls++;
    return t;
}

int counter()
{
    static int n = compute(10);       // a guarded scalar
    return ++n;
}

struct Registry {
    int n = 0;
    void add() { n++; }
};

Registry &registry()
{
    static Registry r;                // constant-initialized: no guard needed
    return r;
}

Tracer third("third");

int main()
{
    printf("main: order %d dyn_int %d plain %d %d baud %d\n", order, dyn_int,
           plain.a, plain.b, cfg.baud);
    printf("names %s %s %s\n", first.name, drivers::second.name, third.name);
    printf("before singleton order %d\n", order);
    Tracer &a = instance();
    Tracer &b = instance();
    printf("singleton same %d seq %d calls %d\n", &a == &b, a.seq, calls);
    int c1 = counter(), c2 = counter(), c3 = counter();
    printf("counter %d %d %d\n", c1, c2, c3);
    registry().add();
    registry().add();
    printf("registry %d\n", registry().n);
    fflush(stdout);
    exit(0);                          // destructors: singleton, third, second, first
}
