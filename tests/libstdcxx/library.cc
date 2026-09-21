// More of libstdc++: algorithms with back_inserter and ostream_iterator,
// <iomanip> manipulators, <random> (mt19937), <regex> (regex_match with
// groups), bitset, valarray, exceptions from vector::at, error codes,
// std::pmr, atomics, std::source_location, <numbers>, numeric_limits,
// map counting.
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory_resource>
#include <numbers>
#include <numeric>
#include <random>
#include <regex>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <valarray>
#include <vector>

int main()
{
    std::vector<int> v(10);
    std::iota(v.begin(), v.end(), 1);
    std::vector<int> out;
    std::copy_if(v.begin(), v.end(), std::back_inserter(out), [](int x) { return x % 3 == 0; });
    std::reverse(out.begin(), out.end());
    auto mm = std::minmax_element(v.begin(), v.end());
    int s1 = std::accumulate(out.begin(), out.end(), 0) + *mm.first + *mm.second;   // 18 + 1 + 10 = 29
    std::ostringstream os;
    std::copy(out.begin(), out.end(), std::ostream_iterator<int>(os, ","));
    os << std::setw(5) << std::setfill('*') << 42 << std::hex << ' ' << 255 << std::fixed
       << std::setprecision(2) << ' ' << 3.14159;
    std::mt19937 gen(42);
    std::uniform_int_distribution<int> dist(1, 6);
    int roll = dist(gen);
    std::regex re("([a-z]+)@([a-z]+)\\.com");
    std::smatch m;
    std::string mail = "user@example.com";
    bool matched = std::regex_match(mail, m, re);
    std::bitset<8> bs(0b1011);
    std::valarray<double> va{1.0, 2.0, 3.0};
    double vs = (va * 2.0).sum();                                  // 12
    int caught = 0;
    try {
        (void)v.at(100);
    } catch (const std::out_of_range &) {
        caught = 1;
    }
    std::error_code ec = std::make_error_code(std::errc::invalid_argument);
    std::pmr::monotonic_buffer_resource pool;
    std::pmr::vector<int> pv(&pool);
    pv.push_back(5);
    std::atomic<int> at{40};
    at.fetch_add(2);
    auto loc = std::source_location::current();
    std::map<std::string, int> wc;
    for (const char *w : {"a", "b", "a"})
        ++wc[w];
    std::printf("%d %s roll=%d match=%d %s %lu %g caught=%d ec=%d pv=%d at=%d line=%u pi=%.3f lim=%d wc=%d\n",
                s1, os.str().c_str(), roll, (int)matched, m[2].str().c_str(), bs.count(), vs, caught,
                ec.value(), pv[0], at.load(), (unsigned)loc.line(), std::numbers::pi,
                std::numeric_limits<short>::max(), wc["a"]);
    return 42;
}
