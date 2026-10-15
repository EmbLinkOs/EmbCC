// C++26's pack indexing: T...[N] names one of a type pack's types and
// args...[i] one of a function parameter pack's values (a value pack's
// too), in an alias template and in a function template's body -- where
// the pack is bound. GCC 16 takes it from C++11 on, and so does EmbCC.
// (In a template's own signature, where the elements are not known yet,
// it is refused by name.)
// expect-exit: 42
template <class... T> using First = T...[0];
template <class... T> using Second = T...[1];
template <class... A> int pick(A... a) { return a...[1] * 10 + a...[0]; }
template <int... N> int third() { return N...[2]; }

int main()
{
    First<int, char> x = 7;
    Second<int, short> y = 3;
    static_assert(sizeof(First<char, int>) == 1, "the first type");
    static_assert(sizeof(Second<char, long long>) == 8, "the second type");
    return pick(2, 4) + (third<5, 6, 7>() - 7) + (x - 7) + (y - 3);   /* 42 */
}
