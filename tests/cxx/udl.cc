// CX6: user-defined literals — cooked (unsigned long long, long double,
// char), raw (const char *) and template (char...) literal operators,
// string literals with their length (also wide, also concatenated),
// mangled as g++ does (li<suffix>); C++14 digit separators and binary
// literals.
// expect-exit: 42
#include <stdio.h>
#include <string.h>
typedef unsigned long size_t;
static int fails;
static void check(const char *what, bool ok)
{
    if (!ok)
        fails++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}
struct Dist { double m; };
constexpr Dist operator""_km(long double d) { return Dist{ (double)d * 1000 }; }
constexpr Dist operator""_km(unsigned long long n) { return Dist{ n * 1000.0 }; }
constexpr unsigned long long operator""_kb(unsigned long long n) { return n * 1024; }
size_t operator""_len(const char *, size_t n) { return n; }
size_t operator""_wlen(const wchar_t *, size_t n) { return n; }
int operator""_raw(const char *s) { return (int)strlen(s); }
int operator""_ch(char c) { return c + 1; }
template <char... Cs> constexpr int operator""_cnt() { return sizeof...(Cs); }
int main()
{
    Dist a = 1.5_km, b = 2_km;
    check("cooked floating and integer", a.m == 1500 && b.m == 2000);
    static_assert(4_kb == 4096, "constexpr");
    check("string", "hello"_len == 5 && L"hi"_wlen == 2);
    check("concatenated", "ab" "cd"_len == 4);
    check("raw", 12345_raw == 5 && 1.25_raw == 4);
    check("character", 'a'_ch == 'b');
    check("template", 123_cnt == 3 && 0x1F_cnt == 4);
    check("digit separators", 1'000'000 == 1000000 && 0x1'00 == 256 && 1'5.0'0 == 15.0);
    check("binary", 0b1010 == 10 && 0b1'0000'0000 == 256 && 0b11u == 3u);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 42;
}
