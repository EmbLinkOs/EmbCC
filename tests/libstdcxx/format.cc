// C++20 <format> and <chrono> through libstdc++: std::format with
// arguments, widths, fill and precision; durations and their arithmetic;
// a calendar date (year_month_day) and its comparisons; <climits>.
#include <chrono>
#include <climits>
#include <format>
#include <iostream>
#include <string>

int main()
{
    std::string s = std::format("{} + {} = {}", 20, 22, 20 + 22);
    std::string t = std::format("[{:>6}] [{:*<5}] [{:.3f}] [{:#x}]", "ab", 7, 3.14159, 255);
    using namespace std::chrono;
    auto d = duration_cast<milliseconds>(seconds(2) + milliseconds(345));
    minutes m = hours(1) + minutes(30);
    year_month_day ymd{year(2024), month(2), day(29)};
    year_month_day next = sys_days(ymd) + days(1);
    std::cout << s << '\n' << t << '\n' << d.count() << ' ' << m.count() << '\n'
              << (int)next.year() << '-' << (unsigned)next.month() << '-'
              << (unsigned)next.day() << ' ' << ymd.ok() << ' ' << (ymd < next)
              << ' ' << (INT_MAX > 0) << '\n';
    return 0;
}
