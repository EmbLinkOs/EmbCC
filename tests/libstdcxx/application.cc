// Application-style C++ over libstdc++: word counting with istringstream
// and map, sorting pairs with a comparator lambda, string surgery (substr,
// transform/toupper, replace, stoi, to_string of a double), unordered_map
// of vectors, CRTP, enable_if overloads, a fold, a user-defined literal,
// a class hierarchy in unique_ptrs with dynamic_cast and typeid, an
// exception type thrown through calls, copies and moves counted.
#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

template <class Derived> struct Printable {
    std::string show() const { return "<" + static_cast<const Derived &>(*this).name() + ">"; }
};
struct Widget : Printable<Widget> { std::string name() const { return "widget"; } };

template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0> std::string kind(T) { return "int"; }
template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0> std::string kind(T) { return "float"; }
template <class... Ts> auto sum(Ts... ts) { return (ts + ... + 0); }
constexpr unsigned long long operator""_kb(unsigned long long v) { return v * 1024; }

struct Animal { virtual ~Animal() = default; virtual std::string sound() const = 0; };
struct Dog : Animal { std::string sound() const override { return "woof"; } };
struct Cat : Animal { std::string sound() const override { return "meow"; } };

struct Parse : std::runtime_error { int at; Parse(int a) : std::runtime_error("parse error"), at(a) {} };
static int parse_digit(char c)
{
    if (!std::isdigit((unsigned char)c))
        throw Parse(c);
    return c - '0';
}

struct Tracker {
    std::string s;
    static int copies, moves;
    Tracker(std::string x) : s(std::move(x)) {}
    Tracker(const Tracker &o) : s(o.s) { copies++; }
    Tracker(Tracker &&o) noexcept : s(std::move(o.s)) { moves++; }
};
int Tracker::copies, Tracker::moves;

int main()
{
    std::string text = "the quick brown fox jumps over the lazy dog the end";
    std::istringstream in(text);
    std::map<std::string, int> freq;
    std::string w;
    while (in >> w)
        ++freq[w];
    std::vector<std::pair<std::string, int>> byfreq(freq.begin(), freq.end());
    std::sort(byfreq.begin(), byfreq.end(), [](auto &a, auto &b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::cout << byfreq[0].first << "=" << byfreq[0].second << " words=" << freq.size() << "\n";
    std::string up = text.substr(4, 5);
    std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    std::string rep = text;
    rep.replace(rep.find("fox"), 3, "cat");
    std::cout << up << " " << rep.substr(10, 9) << " " << std::stoi("42") + std::stoi(" 8") << " "
              << std::to_string(3.5).substr(0, 3) << "\n";
    std::unordered_map<std::string, std::vector<int>> groups;
    for (int i = 0; i < 10; i++)
        groups[i % 2 ? "odd" : "even"].push_back(i);
    std::cout << "odd sum " << std::accumulate(groups["odd"].begin(), groups["odd"].end(), 0) << "\n";
    Widget wd;
    std::cout << wd.show() << " " << kind(1) << kind(2.0) << " " << sum(1, 2, 3, 4) << " " << 4_kb << "\n";
    std::vector<std::unique_ptr<Animal>> zoo;
    zoo.push_back(std::make_unique<Dog>());
    zoo.push_back(std::make_unique<Cat>());
    for (auto &a : zoo)
        std::cout << a->sound() << (dynamic_cast<Dog *>(a.get()) ? "(dog) " : " ");
    std::cout << (typeid(*zoo[1]) == typeid(Cat)) << "\n";
    int total = 0;
    try {
        for (char c : std::string("12x4"))
            total += parse_digit(c);
    } catch (const Parse &p) {
        std::cout << p.what() << " at '" << (char)p.at << "' after " << total << "\n";
    }
    std::vector<Tracker> ts;
    ts.reserve(4);
    Tracker t1("a");
    ts.push_back(t1);
    ts.push_back(std::move(t1));
    ts.emplace_back("c");
    std::cout << "copies " << Tracker::copies << " moves " << Tracker::moves << "\n";
    return 42;
}
