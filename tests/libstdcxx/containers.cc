// More of libstdc++ run: list, deque, set, unordered_map, string_view,
// any, variant, a random engine with a fixed seed (the same numbers as
// g++'s build), iomanip, and designated initializers of a struct.
#include <any>
#include <deque>
#include <iomanip>
#include <iostream>
#include <list>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

struct Options {
    int width = 8;
    char fill = ' ';
    bool hex = false;
};

int main()
{
    std::list<int> l{5, 3, 9};
    l.push_front(1);
    l.sort();
    std::deque<std::string> d{"b", "c"};
    d.push_front("a");
    std::set<int> s{4, 2, 4, 8};
    std::unordered_map<std::string, int> um{{"x", 1}, {"y", 2}};
    std::string_view sv = "hello world";
    std::any a = 42;
    std::variant<int, std::string> v = std::string("var");
    std::mt19937 gen(12345);
    std::uniform_int_distribution<int> dist(1, 100);
    Options o{.width = 6, .fill = '*'};
    for (int x : l)
        std::cout << x << ' ';
    std::cout << '\n' << d.front() << d.back() << ' ' << s.size() << ' '
              << um["y"] << ' ' << sv.substr(6) << ' ' << std::any_cast<int>(a)
              << ' ' << std::get<std::string>(v) << '\n';
    std::cout << gen() << ' ' << gen() << ' ' << dist(gen) << '\n';
    std::cout << std::setw(o.width) << std::setfill(o.fill) << 42 << ' '
              << std::hex << 255 << std::dec << ' ' << o.hex << '\n';
    return 0;
}
