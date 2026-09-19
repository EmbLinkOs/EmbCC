// Class template argument deduction over libstdc++'s own classes and
// guides (pair, tuple, vector from a list and from an iterator range,
// array, optional, map from a list of pairs) — and std::optional, whose
// storage is built through inherited constructors.
#include <array>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

int main()
{
    std::pair p(1, 2.5);
    std::tuple t(1, 'c', std::string("s"));
    std::vector v{3, 1, 2};
    std::vector w(v.begin(), v.end());
    std::array a{1.5, 2.5};
    std::optional o(std::string("opt"));
    std::optional<int> none;
    std::map m{std::pair{1, std::string("one")}, std::pair{2, std::string("two")}};
    std::cout << p.first << ' ' << p.second << ' ' << std::get<2>(t) << ' '
              << v.size() + w.size() << ' ' << a[1] << ' ' << *o << ' '
              << none.value_or(9) << ' ' << m[2] << ' ' << m.size() << '\n';
    return w.back() == 2 ? 0 : 1;
}
