// Much of libstdc++ at once: std::variant and visit (a variant destroyed
// holding each alternative), shared_ptr/weak_ptr/unique_ptr over a class
// hierarchy, std::function, string streams both ways, to_chars/from_chars,
// span, <bit>, optional, any, deque, list, set, unordered_set,
// priority_queue, stack, tuple and apply, string_view.
#include <any>
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <variant>
#include <vector>

struct Shape { virtual ~Shape() = default; virtual int area() const = 0; };
struct Sq : Shape { int s; explicit Sq(int x) : s(x) {} int area() const override { return s * s; } };

int main()
{
    int score = 0;
    std::variant<int, std::string, double> v = std::string("hi");
    score += std::visit([](auto &&x) -> int {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::string>) return (int)x.size();
        else return 0;
    }, v);                                                       // 2
    v = 3.5;
    score += std::holds_alternative<double>(v);                   // 1
    std::shared_ptr<Shape> sp = std::make_shared<Sq>(3);
    std::weak_ptr<Shape> wp = sp;
    score += sp->area() + (int)wp.use_count();                   // 9 + 1
    std::unique_ptr<Shape> up = std::make_unique<Sq>(2);
    score += up->area();                                          // 4
    std::function<int(int)> f = [k = 5](int x) { return x + k; };
    score += f(1);                                                // 6
    std::ostringstream os;
    os << "x=" << 42 << ' ' << 1.5;
    score += os.str() == "x=42 1.5";                              // 1
    std::istringstream is("7 8");
    int a, b;
    is >> a >> b;
    score += a + b;                                               // 15
    char buf[16];
    auto [p, ec] = std::to_chars(buf, buf + 16, 1234);
    score += (int)(p - buf) + (ec == std::errc{});                // 5
    int parsed = 0;
    std::from_chars(buf, p, parsed);
    score += parsed == 1234;                                      // 1
    std::array<int, 4> arr{1, 2, 3, 4};
    std::span<int> sp2(arr);
    score += std::accumulate(sp2.begin(), sp2.end(), 0);          // 10
    score += std::popcount(0xFFu) + std::countr_zero(8u);         // 8 + 3
    std::optional<int> o;
    score += o.value_or(2);                                       // 2
    std::any an = 7;
    score += std::any_cast<int>(an);                              // 7
    std::deque<int> dq{1, 2};
    dq.push_front(0);
    std::list<int> li{3, 1, 2};
    li.sort();
    std::set<int> st{5, 1, 3};
    std::unordered_set<std::string> us{"a", "b"};
    std::priority_queue<int> pq;
    pq.push(3); pq.push(9);
    std::stack<int> sk;
    sk.push(4);
    score += dq.front() + li.front() + *st.begin() + (int)us.size() + pq.top() + sk.top();  // 0+1+1+2+9+4 = 17
    auto tup = std::make_tuple(1, 2.5, 'c');
    score += std::apply([](int x, double y, char z) { return x + (int)y + (z == 'c'); }, tup);  // 4
    {
        /* destroyed holding an int, then a string: only the one alive */
        std::variant<int, std::string> vi = 1, vs = std::string("x");
        score += (int)vi.index() + (int)vs.index();              // 1
    }
    std::string_view sv = "hello world";
    score += (int)sv.find("world");                               // 6
    std::printf("%d\n", score);
    return score == 103 ? 42 : 1;
}
