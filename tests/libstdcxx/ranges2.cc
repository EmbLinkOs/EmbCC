// Range algorithms as libstdc++'s own sources use them (tzdb's rule
// lookup): over built-in arrays (extent_v's partial specializations),
// with a projection by a pointer to data member, comparing strings with
// string_views; ranges::equal_range's subrange (a constructor with a
// `C auto` parameter) made a span<const T>; a span over a non-const array;
// lower_bound, upper_bound, find and sort with projections; and
// std::rotate (std::_V2's, one overload set with pstl's in std).
#include <algorithm>
#include <cstdio>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct Rule {
    std::string name;
    int save;
};

int main()
{
    int fails = 0;
    auto check = [&](const char *what, bool ok) {
        if (!ok)
            fails++;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    };

    std::vector<Rule> rules{{"EU", 1}, {"US", 2}, {"US", 3}, {"Zion", 4}};
    std::span<const Rule> sel =
        std::ranges::equal_range(rules, std::string_view("US"),
                                 std::ranges::less{}, &Rule::name);
    check("equal_range by member, as a span", sel.size() == 2 &&
                                              sel[0].save == 2);

    const int xs[5] = {1, 3, 5, 7, 9};
    auto lb = std::ranges::lower_bound(xs, 5);
    auto ub = std::ranges::upper_bound(xs, 5);
    check("lower_bound/upper_bound on an array", lb - xs == 2 && ub - xs == 3);
    check("std::extent_v", std::extent_v<decltype(xs)> == 5 &&
                           std::extent_v<int[2][3], 1> == 3);

    Rule arr[3] = {{"c", 30}, {"a", 10}, {"b", 20}};
    std::ranges::sort(arr, std::ranges::less{}, &Rule::save);
    std::span<const Rule> all(arr);
    check("sort by member; a span<const T> of a T[N]",
          all.size() == 3 && all[0].name == "a" && all[2].name == "c");
    auto it = std::ranges::find(arr, 20, &Rule::save);
    check("find by member", it != std::end(arr) && it->name == "b");

    std::vector<int> v{1, 2, 3, 4, 5};
    std::rotate(v.begin(), v.begin() + 2, v.end());
    check("std::rotate", v[0] == 3 && v[4] == 2);
    auto r = std::ranges::subrange(v.begin() + 1, v.end() - 1);
    check("subrange", r.size() == 3 && r.front() == 4);

    std::printf("%s\n", fails ? "FAILED" : "all ok");
    return fails;
}
