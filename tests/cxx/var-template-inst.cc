// Explicit instantiation of a variable template: `template int v<int>;`
// defines the instance in this unit whether or not anything here uses it,
// in a namespace too, and with a constant expression for a non-type
// argument; the instance a use names is that one; `extern template`
// leaves a use to read the same value.
// expect-exit: 42
template <class T> T zero = T(0);
template <class T> constexpr T two = T(2);
namespace n {
template <int N> int sq = N * N;
}
template int zero<int>;
template const double two<double>;
template int n::sq<3 + 4>;
extern template long zero<long>;     // another unit's -- here, below

extern "C" int *sq7_address() { return &n::sq<7>; }

template long zero<long>;            // (the definition `extern` promised)

int main()
{
    int fails = 0;
    fails += zero<int> != 0;
    fails += two<double> != 2.0;
    fails += *sq7_address() != 49;
    n::sq<7> += 1;
    fails += n::sq<7> != 50;
    fails += zero<long> != 0;
    return fails == 0 ? 42 : 1;
}
