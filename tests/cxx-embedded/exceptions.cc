// C++ exceptions on a board: throw and catch by value, by reference to a
// base, catch (...), rethrow, a throw from inside a catch, destructors run
// while unwinding (RAII), a constructor that throws after some members
// were built, and a throw through frames that hold values in callee-saved
// registers -- what the unwind tables (.eh_frame, ARM's .ARM.exidx, the
// window and context-save walks of Xtensa and TriCore) and the
// personality routine must agree on. Only the boards whose unwinder
// EmbCC writes run it (embedded-needs below); the host's clang++ build
// prints the same.
// embedded-flags: -fexceptions -frtti
// embedded-needs: exceptions
#include <stdio.h>

static int live;
struct Res {
    int id;
    explicit Res(int i) : id(i) { live++; }
    ~Res() { live--; printf("~Res %d\n", id); }
};

struct Error { int code; };
struct Base { virtual ~Base() {} virtual int what() const { return 1; } };
struct Derived : Base { int what() const override { return 2; } };

__attribute__((noinline)) int thrower(int x)
{
    Res r(x);
    if (x > 2)
        throw Error{x * 10};
    return x;
}

__attribute__((noinline)) int middle(int x, int *acc)
{
    Res r(100 + x);
    int a = *acc + x, b = a * 3;    /* live across the call */
    int t = thrower(x);
    *acc = a + b + t;
    return t;
}

int by_value(int x)
{
    int acc = 1;
    try {
        return middle(x, &acc);
    } catch (Error e) {
        return e.code + acc;
    }
}

int by_base()
{
    try {
        throw Derived();
    } catch (const Base &b) {
        return b.what();
    }
}

int catch_all()
{
    try {
        throw 3.5;
    } catch (int) {
        return -1;
    } catch (...) {
        return 7;
    }
}

int rethrow()
{
    try {
        try {
            throw Error{5};
        } catch (Error &e) {
            e.code++;
            throw;
        }
    } catch (Error e) {
        return e.code;
    }
}

int throw_in_catch()
{
    try {
        try {
            throw 1;
        } catch (int v) {
            Res r(200);
            throw Error{v + 40};
        }
    } catch (Error e) {
        return e.code;
    }
}

struct Pair {
    Res a, b;
    Pair(int x) : a(x), b((x > 0 ? throw Error{x} : 0, x + 1)) {}
};

int ctor_throws()
{
    try {
        Pair p(9);
        return 0;
    } catch (Error e) {
        return e.code;
    }
}

__attribute__((noinline)) int deep(int n)
{
    if (n == 0)
        throw Error{1000};
    Res r(n);
    return deep(n - 1) + 1;
}

int main()
{
    printf("by value %d %d live %d\n", by_value(1), by_value(4), live);
    printf("by base %d\n", by_base());
    printf("catch all %d\n", catch_all());
    printf("rethrow %d\n", rethrow());
    printf("throw in catch %d live %d\n", throw_in_catch(), live);
    printf("ctor throws %d live %d\n", ctor_throws(), live);
    int got = 0;
    for (int n = 1; n <= 6; n++) {
        try {
            deep(n);
        } catch (Error e) {
            got += e.code + n;
        }
    }
    printf("deep %d live %d\n", got, live);
    printf("==END==\n");
    fflush(stdout);
    return 0;
}
