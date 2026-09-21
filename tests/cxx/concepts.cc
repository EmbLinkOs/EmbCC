// CX7 (the core libstdc++ needs): concepts, requires-clauses (after a
// template head, and trailing — on member functions of class templates
// too), requires-expressions (simple, type, compound with noexcept and a
// return-type constraint, nested requirements, parameters), type-
// constraints (template<C T>), constrained placeholders (C auto x), and
// partial specializations chosen by their constraints — the constrained
// one preferred when the patterns tie.
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

template <class T, class U> constexpr bool same = false;
template <class T> constexpr bool same<T, T> = true;
template <class T, class U> concept same_as = same<T, U>;

template <class T> concept Small = sizeof(T) <= 4;
template <class T> concept Addable = requires(T a, T b) { a + b; };
template <class T> concept HasType = requires { typename T::type; };
template <class T> concept Sized = requires(const T &t) {
    { t.size() } noexcept -> same_as<int>;
};
template <class T> concept Both = Small<T> && Addable<T>;
template <class T> concept Either = Small<T> || HasType<T>;

struct Big { char c[16]; };
struct WithType { using type = int; };
struct S1 { int size() const noexcept { return 3; } };
struct S2 { int size() const { return 3; } };
struct S3 { long size() const noexcept { return 3; } };

template <class T, int B = 1> struct pick { static constexpr int v = 0; };
template <class T> requires Small<T> struct pick<T, 1> { static constexpr int v = 1; };
template <class T> requires (!Small<T>) && Addable<T>
struct pick<T, 1> { static constexpr int v = 2; };

template <class T> struct tie { static constexpr int v = 0; };
template <class T> requires HasType<T> struct tie<T> { static constexpr int v = 1; };

template <class T> requires Small<T> int f(T) { return 1; }
template <class T> requires (!Small<T>) int f(T) { return 2; }
template <Addable T> int g(T a) { return (int)(a + a); }
template <class T> int g(T) requires (!Addable<T>) { return -1; }

template <class T> struct Box {
    int get() requires Small<T> { return 4; }
    int get() requires (!Small<T>) { return 8; }
};

enum class E { a };
enum U { u };
template <class T> constexpr bool unscoped = requires(T t, void (*h)(int)) { h(t); };
template <class T> constexpr bool addable = requires(T b) { b + b; };

int main()
{
    check("concepts", Small<int> && !Small<Big> && Addable<int> && !Addable<Big> &&
                      HasType<WithType> && !HasType<int>);
    check("&& and || of concepts", Both<int> && !Both<Big> && Either<WithType> &&
                                   !Either<Big>);
    check("compound requirements", Sized<S1> && !Sized<S2> && !Sized<S3>);
    check("requires-expressions", requires { typename WithType::type; } &&
                                  !addable<Big> && addable<int> &&
                                  requires(int x) { { x + 1 } noexcept; } &&
                                  requires { requires Small<char>; });
    check("parameters' conversions", unscoped<U> && !unscoped<E>);
    check("partial specializations", pick<int>::v == 1 && pick<double>::v == 2 &&
                                     pick<Big>::v == 0);
    check("the constrained one preferred", tie<WithType>::v == 1 && tie<int>::v == 0);
    check("function templates", f('c') == 1 && f(1.0) == 2);
    check("type-constraints", g(21) == 42 && g(Big{}) == -1);
    Box<int> bi;
    Box<Big> bb;
    check("constrained members", bi.get() == 4 && bb.get() == 8);
    Small auto s = 7;
    check("constrained auto", s == 7);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
