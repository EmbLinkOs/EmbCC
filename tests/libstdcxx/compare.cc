// C++20 comparisons through libstdc++: <compare>'s categories, the
// library's operator<=> for vector, string, pair and tuple (with its
// synthesized three-way comparison), a class's defaulted <=> and == over
// a std::string member, std::sort using the rewritten <, and
// std::compare_three_way.
#include <algorithm>
#include <compare>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct Employee {
    std::string name;
    int age;
    auto operator<=>(const Employee &) const = default;
    bool operator==(const Employee &) const = default;
};

static const char *sign(std::strong_ordering o)
{
    return o < 0 ? "less" : o > 0 ? "greater" : "equal";
}

static const char *sign(std::weak_ordering o)
{
    return o < 0 ? "less" : o > 0 ? "greater" : "equivalent";
}

int main()
{
    std::vector<int> a{1, 2, 3}, b{1, 2, 4};
    std::string s = "apple", t = "banana";
    std::pair<int, std::string> p{1, "x"}, q{1, "y"};
    std::tuple<int, double> u{1, 2.0}, w{1, 1.5};
    std::cout << sign(a <=> b) << ' ' << sign(s <=> t) << ' '
              << (p < q) << (u > w) << (a != b) << (s == "apple") << '\n';
    std::vector<Employee> v{{"bob", 30}, {"alice", 25}, {"bob", 20}};
    std::sort(v.begin(), v.end());
    for (const auto &e : v)
        std::cout << e.name << ' ' << e.age << '\n';
    std::cout << sign(std::compare_three_way{}(2, 1)) << ' '
              << (v[0] == Employee{"alice", 25}) << '\n';
    return v[1] < v[2] ? 0 : 1;
}
