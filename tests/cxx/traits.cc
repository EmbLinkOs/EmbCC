// CX8: the type-trait intrinsics libstdc++'s <type_traits> is built on
// (g++'s names), and template forms its headers use: a partial
// specialization for T C::* (C a parameter), an attribute between
// `struct` and a class template's name, default template arguments
// closing with `>>`, an alias template in a base clause, decltype of a
// dependent call in a declaration, bool(...) in a dependent argument.
// expect-exit: 42
#include <stdio.h>

static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

struct Empty {};
struct Pod { int a; char b; };
struct Poly { virtual ~Poly() {} };
struct Abs { virtual void f() = 0; };
struct Fin final { int x; };
struct Der : Pod { int c; };
struct Ctor { Ctor(int) {} };
struct NoCopy { NoCopy() {} NoCopy(const NoCopy &) = delete; };
struct Nothrow { Nothrow() noexcept {} Nothrow(const Nothrow &) {} };
union U { int i; float f; };
enum Plain { P0 };
enum class Scoped : short { S0 };

template <class T> struct remove_cv { using type = __remove_cv(T); };
template <class T> using remove_cv_t = typename remove_cv<T>::type;
template <bool B> struct bool_c { static constexpr bool value = B; };
template <class T> struct helper : bool_c<false> {};
template <> struct helper<int> : bool_c<true> {};
template <class T> struct is_int : helper<remove_cv_t<T>> {};

template <class> struct is_mptr : bool_c<false> {};
template <class T, class C> struct is_mptr<T C::*> : bool_c<true> {};

template <class T>
struct __attribute__((__deprecated__("no"))) old_trait : bool_c<__is_pod(T)> {};

template <class T, class D1 = remove_cv_t<T>, class D2 = remove_cv_t<D1>>
struct twice { using type = D2; };

template <class T> T &&declval();
template <class T> static void take(const T &);
template <class T>
static int probe(const T &, decltype(take<const T &>(declval<T>())) * = 0)
{
    return 1;
}

template <class... B> struct all_true : bool_c<(bool(B::value) && ...)> {};

int main()
{
    check("class/union/enum",
          __is_class(Pod) && !__is_class(U) && __is_union(U) &&
          __is_enum(Plain) && __is_enum(Scoped) && !__is_enum(int));
    check("scoped enum and its type",
          __is_scoped_enum(Scoped) && !__is_scoped_enum(Plain) &&
          __is_same(__underlying_type(Scoped), short));
    check("empty, polymorphic, abstract, final",
          __is_empty(Empty) && !__is_empty(Pod) && __is_polymorphic(Poly) &&
          !__is_polymorphic(Pod) && __is_abstract(Abs) && __is_final(Fin) &&
          !__is_final(Pod));
    check("trivial, pod, standard layout",
          __is_trivial(Pod) && __is_pod(Pod) && !__is_trivial(Poly) &&
          __is_standard_layout(Pod) && !__is_standard_layout(Poly) &&
          __is_trivially_copyable(Pod) && !__is_trivially_copyable(Poly));
    check("destructors", __has_trivial_destructor(Pod) &&
                         !__has_trivial_destructor(Poly) &&
                         __has_virtual_destructor(Poly));
    check("aggregates", __is_aggregate(Pod) && !__is_aggregate(Ctor) &&
                        __is_aggregate(int[3]));
    check("same and base",
          __is_same(int, int) && !__is_same(int, const int) &&
          __is_base_of(Pod, Der) && !__is_base_of(Der, Pod) &&
          __is_base_of(Pod, Pod) && !__is_base_of(int, int));
    check("constructible",
          __is_constructible(Ctor, int) && !__is_constructible(Ctor) &&
          __is_constructible(int) && __is_constructible(Pod, const Pod &) &&
          !__is_constructible(NoCopy, const NoCopy &) &&
          __is_constructible(const int &, int) &&
          !__is_constructible(int &, int));
    check("trivially and nothrow constructible",
          __is_trivially_constructible(Pod) &&
          !__is_trivially_constructible(Ctor, int) &&
          __is_nothrow_constructible(Nothrow) &&
          !__is_nothrow_constructible(Nothrow, const Nothrow &));
    check("assignable",
          __is_assignable(int &, int) && !__is_assignable(int, int) &&
          __is_trivially_assignable(Pod &, const Pod &) &&
          !__is_assignable(const int &, int));
    check("convertible",
          __is_convertible(int, long) && __is_convertible(Der *, Pod *) &&
          !__is_convertible(Pod *, Der *) && __is_convertible(int, Ctor) &&
          !__is_convertible(Ctor, int));
    check("unique representations",
          __has_unique_object_representations(int) &&
          !__has_unique_object_representations(float) &&
          !__has_unique_object_representations(Pod));
    check("type transforms",
          __is_same(__remove_cvref(const int &), int) &&
          __is_same(__decay(int[3]), int *) &&
          __is_same(__add_pointer(int &), int *) &&
          __is_same(__remove_all_extents(int[2][3]), int));
    check("an alias in a base clause", is_int<const int>::value &&
                                       !is_int<long>::value);
    check("T C::* partial specialization", is_mptr<int Pod::*>::value &&
                                           !is_mptr<int *>::value);
    check("attribute before a template's name", old_trait<Pod>::value);
    check("defaults closing with >>",
          __is_same(twice<const int>::type, int));
    check("decltype of a dependent call", probe(5) == 1);
    check("bool(...) in a dependent argument",
          all_true<bool_c<true>, is_int<int>>::value &&
          !all_true<bool_c<true>, is_int<char>>::value);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
