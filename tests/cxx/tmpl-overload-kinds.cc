// Two function templates of one name whose function parameters are the
// same and whose template parameters are of other kinds -- std::get<I>
// and std::get<T> of a variant -- are two templates. Compared by the
// function parameters alone, the second's body was taken for the first's
// definition, and a call written inside the class template found only one.
// expect-exit: 42
typedef unsigned long size_t;
namespace ns {
template <typename... T> struct var;
template <size_t N, typename V> struct alt { typedef int type; };
template <size_t N, typename V> using alt_t = typename alt<N, V>::type;
template <size_t N, typename... T> constexpr alt_t<N, var<T...>> &get(var<T...> &);
template <typename U, typename... T> constexpr U &get(var<T...> &v) { return v.y; }
template <typename... T> struct var {
    int x, y;
    template <typename U> var &operator=(U &&r) {
        constexpr auto idx = sizeof(U) > 100 ? 1 : 0;
        ns::get<idx>(*this) = r;            // the index form
        return *this;
    }
};
template <size_t N, typename... T> constexpr alt_t<N, var<T...>> &get(var<T...> &v) { return v.x; }
}
int main()
{
    ns::var<int, double> v{0, 0};
    v = 40;
    ns::get<int>(v) = 2;                    // the type form
    return ns::get<0>(v) + v.y;             // 42
}
