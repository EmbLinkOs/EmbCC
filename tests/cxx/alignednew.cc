// C++17 aligned allocation and what libsupc++'s own sources needed: new
// and delete of an over-aligned type go to operator new/delete(...,
// std::align_val_t) — the class's own first, arrays and deleting
// destructors too; alignas and __attribute__((aligned)) on members, laid
// out as g++ does, and __alignof__ of such a member (a flexible array's);
// ?: between a pointer to member and nullptr; X::operator T() defined
// outside X with T X's member typedef; __constinit; &f of an overloaded f
// passed where a function pointer is wanted.
// expect-exit: 42
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

namespace std {
enum class align_val_t : size_t {};                // as <new> declares it
}
void *operator new(size_t, std::align_val_t);      // libsupc++'s
void operator delete(void *, std::align_val_t) noexcept;
void operator delete(void *, size_t, std::align_val_t) noexcept;
void *operator new[](size_t, std::align_val_t);
void operator delete[](void *, size_t, std::align_val_t) noexcept;

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static bool aligned(const void *p, size_t a) { return reinterpret_cast<uintptr_t>(p) % a == 0; }

struct alignas(64) Big { char c[3]; virtual ~Big() {} };
struct alignas(32) Vec4 { double d[4]; ~Vec4() {} };
static int own_news, own_deletes;
struct alignas(128) Own {
    int v;
    static void *operator new(size_t n, std::align_val_t al) { own_news++; return ::operator new(n, al); }
    static void operator delete(void *p, std::align_val_t al) { own_deletes++; ::operator delete(p, al); }
};

struct Members { char c; alignas(16) char d; int e; };
struct Flex { int n; char data[] __attribute__((aligned)); };
struct Wide { char x; long y __attribute__((aligned(32))); };

struct Handle {
    typedef void (Handle::*safe_bool)();
    void dummy() {}
    void *p;
    operator safe_bool() const;
};
Handle::operator safe_bool() const { return p ? &Handle::dummy : nullptr; }

static __constinit int counter = 40;

static int apply(int (*f)(int), int v) { return f(v); }
static int twice(int x) { return 2 * x; }
static double twice(double x) { return 2 * x; }

int main()
{
    Big *b = new Big;
    Vec4 *v = new Vec4[3];
    Own *o = new Own;
    check("over-aligned new", aligned(b, 64) && aligned(v, 32) && aligned(o, 128));
    delete b;
    delete[] v;
    delete o;
    check("the class's own aligned operators", own_news == 1 && own_deletes == 1);
    check("aligned members", sizeof(Members) == 32 && offsetof(Members, d) == 16 &&
                             sizeof(Flex) == 16 && __alignof__(((Flex *)0)->data) == 16 &&
                             sizeof(Wide) == 64 && offsetof(Wide, y) == 32);
    Handle h{&counter}, n{nullptr};
    check("?: of a pointer to member and nullptr", h && !n);
    counter += 2;
    check("__constinit, &overloaded", counter == 42 && apply(&twice, 21) == 42);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
