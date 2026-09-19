// libstdc++'s red-black tree: std::map with string and int keys —
// operator[] (piecewise construction through tuples), insert of a braced
// pair, ordered iteration, structured bindings over the elements, count.
#include <iostream>
#include <map>
#include <string>

int main()
{
    std::map<std::string, int> m;
    m["one"] = 1;
    m["two"] = 2;
    m.insert({ "three", 3 });
    for (auto &kv : m)
        std::cout << kv.first << '=' << kv.second << '\n';
    std::map<int, int> sq;
    for (int i = 0; i < 5; i++)
        sq[i] = i * i;
    int s = 0;
    for (const auto &[k, v] : sq)
        s += v;
    std::cout << "sum " << s << " size " << sq.size() << '\n';
    return m.count("two") ? 0 : 1;
}
