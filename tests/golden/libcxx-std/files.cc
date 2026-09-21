/* <fstream> and <variant>: real files through our own FILE, and a tagged
 * union that knows how to destroy itself. */
#include <fstream>
#include <sstream>
#include <variant>
#include <string>
#include <cstdio>
#include <vector>
#include "check.h"
using namespace std;

static int live;
struct R { int v; R(int x = 0) : v(x) { live++; }
           R(const R &o) : v(o.v) { live++; }
           R(R &&o) noexcept : v(o.v) { live++; }
           ~R() { live--; }
           bool operator==(const R &o) const { return v == o.v; } };

int main()
{
    const char *path = "libcxx-std-test.txt";

    /* This target may have no filesystem at all -- the QEMU harness has
     * an `open` that returns ENOSYS -- and on one that does not, the
     * correct behaviour of every call below is to FAIL. So the file
     * checks are conditional on a probe, and the probe itself is the
     * first thing asserted: a stream that could not open must report
     * !is_open() and failbit, not a usable stream.
     *
     * The same programs run against EmbLinkOS, which does have files. */
    bool has_files;
    {
        ofstream probe(path);
        has_files = probe.is_open();
        if (!has_files) {
            CHECK(!probe);              /* failbit, not a usable stream */
            CHECK(probe.fail());
            printf("(no filesystem on this target: file checks skipped)\n");
        }
    }

    /* ---- write, then read back ---- */
    if (has_files) {
        ofstream out(path);
        CHECK(out.is_open());
        CHECK(!!out);
        out << "line one\n" << 42 << ' ' << 2.5 << '\n';
        out << "last\n";
        out.close();
    }
    if (has_files) {
        ifstream in(path);
        CHECK(in.is_open());
        string l1;
        getline(in, l1);
        CHECK(l1 == "line one");
        int n = 0; double d = 0;
        in >> n >> d;
        CHECK(n == 42 && d == 2.5);
        string rest, last;
        getline(in, rest);               /* the rest of the number line */
        CHECK(rest == "");
        getline(in, last);
        CHECK(last == "last");
        /* One more read finds the end. */
        string none;
        CHECK(!getline(in, none));
        CHECK(in.eof());
    }

    /* Reading a whole file: the loop everyone writes. */
    if (has_files) {
        ifstream in(path);
        vector<string> lines;
        string l;
        while (getline(in, l)) lines.push_back(l);
        CHECK(lines.size() == 3);
        CHECK(lines[0] == "line one" && lines[2] == "last");
    }

    /* A file bigger than the buffer, so refilling the get window is
     * exercised rather than assumed. */
    if (has_files) {
        {
            ofstream out(path);
            for (int i = 0; i < 2000; i++) out << i << '\n';
        }
        ifstream in(path);
        int seen = 0, v;
        while (in >> v) { CHECK(v == seen); seen++; }
        CHECK(seen == 2000);
    }

    /* Appending adds rather than truncating. */
    if (has_files) {
        { ofstream out(path); out << "a\n"; }
        { ofstream out(path, ios_base::out | ios_base::app); out << "b\n"; }
        ifstream in(path);
        string x, y;
        getline(in, x); getline(in, y);
        CHECK(x == "a" && y == "b");
    }

    /* A failed open sets failbit rather than throwing, so `if (!f)` is
     * the test -- which is what every caller writes. */
    {
        ifstream missing("no-such-file-here-at-all.txt");
        CHECK(!missing.is_open());
        CHECK(!missing);
        CHECK(missing.fail());
    }

    if (has_files) remove(path);

    /* ---- variant ---- */
    {
        variant<int, string> v = 42;
        CHECK(v.index() == 0);
        CHECK(holds_alternative<int>(v) && !holds_alternative<string>(v));
        CHECK(get<int>(v) == 42);
        CHECK(*get_if<int>(&v) == 42);
        CHECK(get_if<string>(&v) == nullptr);

        v = string("hello");
        CHECK(v.index() == 1);
        CHECK(get<string>(v) == "hello");
        CHECK(get<1>(v) == "hello");

        /* get CHECKS -- a variant's alternative can change, so an
         * unchecked get would be type confusion rather than merely a
         * null dereference. */
        int caught = 0;
        try { (void)get<int>(v); }
        catch (const bad_variant_access &) { caught = 1; }
        CHECK(caught == 1);
    }

    /* Changing the alternative destroys the old one exactly once. */
    {
        {
            variant<R, string> v(R(1));
            CHECK(live == 1 && get<R>(v).v == 1);
            v = string("now a string");
            CHECK(live == 0);
            v = R(2);
            CHECK(live == 1 && get<R>(v).v == 2);
            variant<R, string> c = v;
            CHECK(live == 2);
            variant<R, string> m = std::move(c);
            CHECK(get<R>(m).v == 2);
        }
        CHECK(live == 0);
    }

    /* emplace, comparison, and visit. */
    {
        variant<int, string> a, b;
        a.emplace<string>(3, 'z');
        CHECK(get<string>(a) == "zzz");
        b = string("zzz");
        CHECK(a == b);
        b = 1;
        CHECK(a != b);

        /* visit calls the one function matching whichever alternative is
         * held -- the alternative to a chain of holds_alternative tests. */
        struct Vis {
            string operator()(int i) const { return "int:" + to_string(i); }
            string operator()(const string &s) const { return "str:" + s; }
        };
        variant<int, string> x = 7;
        CHECK(visit(Vis{}, x) == "int:7");
        x = string("q");
        CHECK(visit(Vis{}, x) == "str:q");
    }
    DONE();
}
