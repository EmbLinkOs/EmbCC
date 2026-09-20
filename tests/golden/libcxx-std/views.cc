/* <string_view>, <span>, <bitset>: the non-owning views, and the proxy
 * reference. */
#include <string_view>
#include <span>
#include <bitset>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <functional>
#include "check.h"
using namespace std;

static size_t count_words(string_view s)
{
    size_t n = 0;
    while (!s.empty()) {
        size_t sp = s.find(' ');
        if (sp == string_view::npos) { n++; break; }
        if (sp > 0) n++;
        s.remove_prefix(sp + 1);           /* moves the window; no copy */
    }
    return n;
}

int main()
{
    /* ---- string_view ---- */
    {
        string_view sv = "hello world";
        CHECK(sv.size() == 11 && !sv.empty());
        CHECK(sv[0] == 'h' && sv.front() == 'h' && sv.back() == 'd');
        CHECK(sv.substr(6) == "world");
        CHECK(sv.substr(0, 5) == "hello");
        CHECK(sv.find("world") == 6);
        CHECK(sv.find('o') == 4);
        CHECK(sv.find("zzz") == string_view::npos);
        CHECK(sv.starts_with("hello") && sv.ends_with("world"));
        CHECK(sv.contains("lo w"));

        /* It owns nothing: a view of a string points INTO it. */
        string owner = "abcdef";
        string_view v(owner);
        CHECK(v.data() == owner.data());
        CHECK(v == "abcdef");
        CHECK(v.size() == owner.size());

        /* remove_prefix/suffix move the window and copy nothing, which
         * is what makes tokenising allocation-free. */
        string_view w = "abcdef";
        w.remove_prefix(2);
        w.remove_suffix(1);
        CHECK(w == "cde");
        CHECK(count_words("one two three") == 3);
        CHECK(count_words("a") == 1);

        CHECK(string_view("ab") < string_view("abc"));
        CHECK(string_view("abc") == string(("abc")));
        string_view empty;
        CHECK(empty.empty() && empty.size() == 0);
    }

    /* ---- span: the same non-ownership, but mutable ---- */
    {
        int raw[5] = {1, 2, 3, 4, 5};
        span<int> s(raw);
        CHECK(s.size() == 5 && s.data() == raw);
        CHECK(s[0] == 1 && s.front() == 1 && s.back() == 5);

        /* Writing through the span writes through to the array: a span
         * is how a function says "I will read and write these N and not
         * resize them". */
        s[0] = 99;
        CHECK(raw[0] == 99);

        CHECK(s.first(2).size() == 2 && s.first(2)[1] == 2);
        CHECK(s.last(2)[0] == 4);
        CHECK(s.subspan(1, 3).size() == 3 && s.subspan(1, 3)[0] == 2);
        CHECK(s.size_bytes() == 5 * sizeof(int));

        int sum = 0;
        for (int x : s) sum += x;
        CHECK(sum == 99 + 2 + 3 + 4 + 5);

        /* Over a vector and over an array, the same way. */
        vector<int> v{10, 20, 30};
        span<int> vs(v);
        CHECK(vs.size() == 3 && vs[2] == 30);
        vs[2] = 31;
        CHECK(v[2] == 31);

        array<int, 3> a{7, 8, 9};
        span<int> as(a);
        CHECK(as.size() == 3 && as[0] == 7);

        /* An algorithm over a span is an algorithm over the original. */
        sort(vs.begin(), vs.end(), greater<int>{});
        CHECK(v[0] == 31 && v[2] == 10);
    }

    /* ---- bitset ---- */
    {
        bitset<8> b;
        CHECK(b.size() == 8 && b.none() && b.count() == 0);
        b.set(0);
        b.set(3);
        CHECK(b.count() == 2 && b.any() && !b.all());
        CHECK(b.test(0) && b.test(3) && !b.test(1));
        CHECK(b.to_string() == "00001001");
        CHECK(b.to_ulong() == 9);

        /* operator[] on a mutable bitset returns a PROXY, because a bit
         * has no address for a bool& to point at. */
        b[1] = true;
        CHECK(b.test(1) && b.count() == 3);
        b[1] = b[7];
        CHECK(!b.test(1));
        CHECK((bool)b[0] == true);
        b[0].flip();
        CHECK(!b.test(0));

        b.reset();
        b.set();
        CHECK(b.all() && b.count() == 8);
        /* The bits above N must stay zero or count() and all() lie. */
        b.flip();
        CHECK(b.none() && b.count() == 0);
    }
    {
        /* A size that is not a multiple of the word, so the trimming of
         * the top word is exercised. */
        bitset<70> b;
        b.set();
        CHECK(b.count() == 70 && b.all());
        b.flip();
        CHECK(b.count() == 0);
        b.set(69);
        CHECK(b.count() == 1 && b.test(69) && !b.test(68));
        b.reset(69);
        CHECK(b.none());
    }
    {
        bitset<8> a(string("1010"));
        /* The string reads most-significant first -- the opposite of the
         * index order. */
        CHECK(a.test(1) && a.test(3) && !a.test(0) && !a.test(2));
        CHECK(a.to_ulong() == 10);

        bitset<8> x(0xF0ULL), y(0x0FULL);
        CHECK((x & y).none());
        CHECK((x | y).all());
        CHECK((x ^ y).all());
        CHECK((~x) == y);
        CHECK((y << 4) == x);
        CHECK((x >> 4) == y);
    }
    DONE();
}
