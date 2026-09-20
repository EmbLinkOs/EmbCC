/* <string>: the small-string optimisation, and what it makes easy to get
 * wrong. */
#include <string>
#include <stdexcept>
#include "check.h"
using namespace std;

int main()
{
    /* Short strings live inside the object: no allocation, and data()
     * points into the string itself. */
    string s = "hello";
    CHECK(s.size() == 5 && !s.empty());
    CHECK(s.data() >= (const char *)&s);
    CHECK(s.data() < (const char *)&s + sizeof s);
    CHECK(s.capacity() >= 15);
    CHECK(s[0] == 'h' && s.at(4) == 'o' && s.front() == 'h' && s.back() == 'o');
    CHECK(s.c_str()[5] == '\0');            /* always NUL-terminated */

    /* Long strings do allocate, and data() then points outside. */
    string big(100, 'x');
    CHECK(big.size() == 100 && big.capacity() >= 100);
    CHECK(big.data() < (const char *)&big ||
          big.data() >= (const char *)&big + sizeof big);
    CHECK(big[99] == 'x' && big.c_str()[100] == '\0');

    /* COPYING A SHORT STRING must re-aim the pointer at the new object's
     * own buffer. Copying the three words blindly leaves the copy
     * pointing into the original -- which still works until the original
     * dies, which is what makes the bug so hard to find. */
    {
        string a = "short";
        string b = a;
        CHECK(b == "short");
        CHECK(b.data() != a.data());        /* its OWN buffer */
        a = "changed";
        CHECK(b == "short");                /* and unaffected */
    }
    /* Same for moving one. */
    {
        string a = "short";
        string b = move(a);
        CHECK(b == "short");
        CHECK(b.data() >= (const char *)&b);
        CHECK(b.data() < (const char *)&b + sizeof b);
    }
    /* A long string really is stolen, not copied. */
    {
        string a(100, 'y');
        const char *p = a.data();
        string b = move(a);
        CHECK(b.data() == p);
        CHECK(a.empty());                   /* and left usable */
        a = "reused";
        CHECK(a == "reused");
    }

    /* Growing across the short/long boundary keeps the contents. */
    {
        string g;
        for (int i = 0; i < 40; i++) g.push_back((char)('a' + i % 26));
        CHECK(g.size() == 40);
        CHECK(g[0] == 'a' && g[25] == 'z' && g[26] == 'a');
        CHECK(g.c_str()[40] == '\0');
    }

    /* append, +=, +, insert, erase, replace */
    {
        string a = "abc";
        a += "def";
        a.append("gh", 2);
        a.append(2, '!');
        CHECK(a == "abcdefgh!!");
        CHECK((string("x") + "y" + 'z') == "xyz");
        CHECK(("p" + string("q")) == "pq");
        a.insert(3, "-");
        CHECK(a == "abc-defgh!!");
        a.erase(3, 1);
        CHECK(a == "abcdefgh!!");
        a.erase(8);
        CHECK(a == "abcdefgh");
        a.pop_back();
        CHECK(a == "abcdefg");
    }

    /* Self-append has to survive the buffer moving underneath it. */
    {
        string a = "0123456789abcde";        /* exactly the short capacity */
        a += a;
        CHECK(a.size() == 30);
        CHECK(a.substr(0, 15) == "0123456789abcde");
        CHECK(a.substr(15) == "0123456789abcde");
    }

    /* searching */
    {
        string h = "the quick brown fox";
        CHECK(h.find("quick") == 4);
        CHECK(h.find("slow") == string::npos);
        CHECK(h.find('q') == 4);
        CHECK(h.rfind('o') == 17);
        CHECK(h.find("o") == 12);
        CHECK(h.find_first_of(string("xyz")) == 18);
        CHECK(h.starts_with("the") && !h.starts_with("he"));
        CHECK(h.ends_with("fox") && h.contains("brown"));
        CHECK(h.substr(4, 5) == "quick");
    }

    /* Ordering compares the shorter length first, so "ab" < "abc". */
    {
        CHECK(string("ab") < string("abc"));
        CHECK(string("abc") > string("ab"));
        CHECK(string("a") < string("b"));
        CHECK(string("abc") == "abc");
        CHECK(string("abc") != "abd");
    }

    /* swap works whatever combination of short and long. */
    {
        string a = "sh", b(100, 'L');
        a.swap(b);
        CHECK(a.size() == 100 && a[0] == 'L');
        CHECK(b == "sh");
        b.swap(a);
        CHECK(b.size() == 100 && a == "sh");
    }

    /* at() checks; operator[] does not. */
    {
        int caught = 0;
        try { (void)s.at(99); } catch (const out_of_range &) { caught = 1; }
        CHECK(caught == 1);
    }

    /* The numeric conversions, and the two failures they must report. */
    {
        CHECK(to_string(42) == "42");
        CHECK(to_string(-7L) == "-7");
        CHECK(to_string(18446744073709551615ULL) == "18446744073709551615");
        CHECK(to_string(1.5).substr(0, 3) == "1.5");
        CHECK(stoi("42") == 42);
        CHECK(stoi("-2a") == -2);
        CHECK(stol("0x2a", nullptr, 16) == 42);
        CHECK(stod("2.5") == 2.5);
        size_t pos = 0;
        CHECK(stoi("17rest", &pos) == 17 && pos == 2);
        int bad = 0;
        try { (void)stoi("zzz"); } catch (const invalid_argument &) { bad = 1; }
        CHECK(bad == 1);
        int over = 0;
        try { (void)stoi("99999999999999999999"); }
        catch (const out_of_range &) { over = 1; }
        CHECK(over == 1);
        /* Out of int's range but inside long's: stoi must still refuse. */
        int narrow = 0;
        try { (void)stoi("3000000000"); }
        catch (const out_of_range &) { narrow = 1; }
        CHECK(narrow == 1);
    }

    /* Iteration, and that a string is a range. */
    {
        string a = "abc";
        int n = 0;
        for (char c : a) n = n * 10 + (c - 'a');
        CHECK(n == 12);
        CHECK(*a.rbegin() == 'c');
        CHECK(a.end() - a.begin() == 3);
        string b(a.begin(), a.end());
        CHECK(b == "abc");
    }
    DONE();
}
