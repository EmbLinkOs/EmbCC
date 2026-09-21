// C++20 <ranges> and <functional> through libstdc++: iota views (over
// unsigned long long too: __max_size_type's conversion templates), filter,
// transform, reverse and take — called, partially applied and piped (the
// adaptors' operator() takes an explicit object parameter), composed
// pipelines, ranges::size, find_if, sort and count_if; std::bind with
// placeholders, bind_front and not_fn.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <ranges>
#include <vector>

static int sub(int a, int b) { return a - b; }

int main()
{
    long long s = 0;
    for (auto x : std::views::iota(0ULL, 10ULL))
        s += (long long)x;
    auto r = std::views::iota(1LL, 6LL);
    auto n = std::ranges::size(r);
    std::vector<int> v{5, 3, 9, 1, 7};
    auto big = [](int x) { return x > 2; };
    auto ev = v | std::views::filter(big) | std::views::transform([](int x) { return x * 10; });
    int t = 0;
    for (int x : ev)
        t += x;
    auto part = std::views::filter(big);
    int t2 = 0;
    for (int x : part(v))
        t2 += x;
    auto it = std::ranges::find_if(v, [](int x) { return x > 8; });
    std::ranges::sort(v);
    auto last2 = std::views::reverse | std::views::take(2);
    int top = 0;
    for (int x : v | last2)
        top = top * 100 + x;
    std::printf("iota %lld size %zu filtered %d %d found %d top %d\n", s, (size_t)n, t,
                t2, *it, top);

    using namespace std::placeholders;
    auto minus10 = std::bind(sub, _1, 10);
    auto flip = std::bind(sub, _2, _1);
    auto even = std::not_fn([](int x) { return x % 2 != 0; });
    int evens = (int)std::ranges::count_if(std::views::iota(1, 7), even);
    auto from100 = std::bind_front(sub, 100);
    std::printf("bind %d %d not_fn %d bind_front %d\n", minus10(52), flip(1, 43), evens,
                from100(58));
    return s == 45 && n == 5 && t == 240 && t2 == 24 && top == 907 && evens == 3 ? 42 : 1;
}
