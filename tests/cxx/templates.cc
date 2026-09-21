// CX4: templates — class templates (type, value and template template
// parameters, defaults), members instantiated when used and defined out of
// the class, static members, nested classes; function templates with
// deduction (T, T*, const T&, T&&, T(&)[N], A<T>) and explicit arguments,
// preferred less than an equally good non-template; explicit and partial
// specializations, matched exactly and the most specialized chosen; member
// templates; alias and variable templates; dependent names; recursion;
// CRTP; SFINAE.
// expect-exit: 42
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- class templates ----
template <class T, int N = 4>
struct Array {
    T data[N];
    int size() const { return N; }
    T &operator[](int i) { return data[i]; }
    T sum() const;
    static int instances;
    struct Iter {
        const T *p;
        T get() const { return *p; }
    };
    Iter first() const { return Iter{ data }; }
};
template <class T, int N>
T Array<T, N>::sum() const
{
    T s = T();
    for (int i = 0; i < N; i++)
        s += data[i];
    return s;
}
template <class T, int N>
int Array<T, N>::instances = 7;

template <class T>
struct Pair {
    T a, b;
    template <class U>
    U convert_sum() const { return (U)a + (U)b; }
};

template <template <class> class Holder, class T>
struct Wrap {
    Holder<T> h;
};

// ---- function templates ----
template <class T> const char *kind(T) { return "T"; }
template <class T> const char *kind(T *) { return "T*"; }
const char *kind(int) { return "int"; }

template <class T> T maxof(const T &a, const T &b) { return a < b ? b : a; }
template <class T, int N> int count(T (&)[N]) { return N; }
template <class T> T first_of(const Pair<T> &p) { return p.a; }
template <class T> int is_lref(T &&) { return 0; }
template <class T> int is_lref(T &) { return 1; }

// ---- specializations ----
template <class T> struct Name { static const char *get() { return "other"; } };
template <> struct Name<int> { static const char *get() { return "int"; } };
template <class T> struct Name<T *> { static const char *get() { return "pointer"; } };
template <class T> struct Name<Pair<T>> { static const char *get() { return "pair"; } };

template <class A, class B> struct Which { static const int v = 0; };
template <class T> struct Which<int, T> { static const int v = 1; };
template <class T> struct Which<T, T> { static const int v = 2; };
template <class T> struct Which<T *, int> { static const int v = 3; };
template <class T> struct Which<T *, T *> { static const int v = 4; };
template <class T> struct Cv { static const int v = 0; };
template <class T> struct Cv<const T> { static const int v = 1; };
template <class T> struct Cv<T *const> { static const int v = 2; };

template <class T> int weight(T) { return 1; }
template <> int weight<char>(char) { return 100; }

// ---- aliases, variables, dependent names ----
template <class T> using PairOf = Pair<T>;
template <class T> constexpr int bits = sizeof(T) * 8;

struct HasType { typedef long type; };
template <class T> typename T::type make_type() { return 5; }

// ---- recursion and CRTP ----
template <int N> struct Fact { static const long value = N * Fact<N - 1>::value; };
template <> struct Fact<0> { static const long value = 1; };

template <class D> struct Shape {
    int area() const { return static_cast<const D *>(this)->w * 2; }
};
struct Sq : Shape<Sq> { int w = 21; };

// ---- SFINAE ----
template <bool B, class T = void> struct enable_if {};
template <class T> struct enable_if<true, T> { typedef T type; };
template <class T> struct is_ptr { static const bool value = false; };
template <class T> struct is_ptr<T *> { static const bool value = true; };

template <class T>
typename enable_if<is_ptr<T>::value, int>::type pick(T) { return 1; }
template <class T>
typename enable_if<!is_ptr<T>::value, int>::type pick(T) { return 2; }

int main()
{
    Array<int, 3> a = { { 1, 2, 3 } };
    a[1] = 10;
    check("class template, members used", a.size() == 3 && a.sum() == 14);
    Array<double> d = { { 0.5, 0.5, 1, 2 } };
    check("default template argument", d.size() == 4 && d.sum() == 4.0);
    check("static member of a template", Array<int, 3>::instances == 7);
    check("nested class of a template", a.first().get() == 1);
    Pair<int> p = { 3, 4 };
    check("member template", p.convert_sum<double>() == 7.0);
    Wrap<Pair, char> w;
    w.h.a = 'x';
    check("template template parameter", w.h.a == 'x');

    int x = 1;
    check("overloads: non-template preferred", strcmp(kind(1), "int") == 0);
    check("overloads: more specialized T*", strcmp(kind(&x), "T*") == 0);
    check("overloads: T", strcmp(kind(1.5), "T") == 0);
    check("deduction through const T&", maxof(3, 9) == 9 &&
                                        maxof<long>(3, 9L) == 9);
    int arr[5];
    check("deduction of an array bound", count(arr) == 5);
    check("deduction through A<T>", first_of(p) == 3);
    check("partial ordering: T& over T&&", is_lref(x) == 1);

    check("explicit specialization", strcmp(Name<int>::get(), "int") == 0);
    check("partial specialization", strcmp(Name<int *>::get(), "pointer") == 0 &&
                                    strcmp(Name<Pair<long>>::get(), "pair") == 0 &&
                                    strcmp(Name<char>::get(), "other") == 0);
    check("partial specializations match exactly",
          Which<long, char>::v == 0 && Which<int, char>::v == 1 &&
          Which<char, char>::v == 2 && Which<char *, int>::v == 3 &&
          Cv<int>::v == 0 && Cv<int *>::v == 0);
    check("the most specialized partial specialization",
          Which<char *, char *>::v == 4 && Which<int *, int *>::v == 4 &&
          Cv<const int>::v == 1 && Cv<int *const>::v == 2);
    check("function specialization", weight('a') == 100 && weight(1) == 1);

    PairOf<short> ps = { 1, 2 };
    check("alias template", sizeof(ps) == 2 * sizeof(short));
    check("variable template", bits<int> == 32 && bits<char> == 8);
    check("typename T::type", make_type<HasType>() == 5);
    check("recursive instantiation", Fact<10>::value == 3628800);
    Sq s;
    check("CRTP", s.area() == 42);
    check("SFINAE", pick(&x) == 1 && pick(x) == 2);

    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
