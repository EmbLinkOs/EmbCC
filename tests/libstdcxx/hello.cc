// libstdc++ used as is: std::vector, std::string, std::sort, std::cout —
// compiled by EmbCC against libstdc++'s own headers, linked with g++'s
// libstdc++.a.
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

int main()
{
    std::vector<int> v{ 3, 1, 2 };
    std::sort(v.begin(), v.end());
    std::string s = "hello";
    s += " world";
    std::cout << s << " " << v.size() << " " << v[0] << v[1] << v[2]
              << std::endl;
    std::vector<std::string> words{ "pear", "apple", "fig" };
    std::sort(words.begin(), words.end());
    for (const std::string &w : words)
        std::cout << w << (w == words.back() ? "\n" : ",");
    return v[2] == 3 && s.size() == 11 && words[0] == "apple" ? 42 : 1;
}
