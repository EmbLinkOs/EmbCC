/* <iostream>, <sstream>, <iomanip>: formatted I/O on our own streams. */
#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include "check.h"
using namespace std;

int main()
{
    /* ---- ostringstream: the formatting, without a file in the way ---- */
    {
        ostringstream o;
        o << "n=" << 42 << " f=" << 1.5 << " c=" << 'x' << " s=" << string("str");
        CHECK(o.str() == "n=42 f=1.5 c=x s=str");
    }
    {
        ostringstream o;
        o << true << ' ' << false;
        CHECK(o.str() == "1 0");
        ostringstream b;
        b << boolalpha << true << ' ' << false;
        CHECK(b.str() == "true false");
    }
    {
        ostringstream o;
        o << hex << 255 << ' ' << oct << 8 << ' ' << dec << 10;
        CHECK(o.str() == "ff 10 10");
        ostringstream u;
        u << showbase << hex << 255u;
        CHECK(u.str() == "0xff");
    }

    /* WIDTH IS CONSUMED by the next insertion; fill and precision are
     * sticky. This is the asymmetry that surprises everyone once. */
    {
        ostringstream o;
        o << setw(5) << 42 << 42;
        CHECK(o.str() == "   4242");        /* the second 42 is not padded */
        ostringstream f;
        f << setfill('0') << setw(5) << 42 << ' ' << setw(3) << 7;
        CHECK(f.str() == "00042 007");      /* fill persisted */
        ostringstream l;
        l << left << setw(5) << 42 << '|';
        CHECK(l.str() == "42   |");
    }
    {
        ostringstream o;
        o << fixed << setprecision(2) << 3.14159 << ' ' << 2.0;
        CHECK(o.str() == "3.14 2.00");      /* precision persisted */
        ostringstream s;
        s << scientific << setprecision(1) << 1234.0;
        CHECK(s.str().find("1.2e+03") != string::npos);
    }

    /* ---- istringstream ---- */
    {
        istringstream i("10 20 hello 2.5");
        int a = 0, b = 0; string w; double d = 0;
        i >> a >> b >> w >> d;
        CHECK(a == 10 && b == 20 && w == "hello" && d == 2.5);
        CHECK(!!i);                          /* still good */
        int extra = -1;
        i >> extra;
        CHECK(i.fail());                     /* nothing left */
        CHECK(extra == -1);                  /* and the value is untouched */
    }

    /* The loop condition is the stream, not eof(): a read that succeeds
     * and happens to reach the end is still true, so testing eof()
     * instead processes the last item twice. */
    {
        istringstream i("1 2 3");
        vector<int> v;
        int x;
        while (i >> x) v.push_back(x);
        CHECK(v.size() == 3);
        CHECK(v[0] == 1 && v[2] == 3);
        CHECK(i.eof());
    }

    /* The sentry skips leading whitespace, including newlines -- which
     * is why `>>` reads across lines without anyone asking it to. */
    {
        istringstream i("  \n\t 7   \n 8 ");
        int a = 0, b = 0;
        i >> a >> b;
        CHECK(a == 7 && b == 8);
    }
    {
        istringstream i(" ab");
        char c = 0;
        i >> noskipws >> c;
        CHECK(c == ' ');                     /* not skipped */
    }

    /* getline does not skip, does not store the delimiter, and DOES
     * consume it -- which is why mixing >> and getline leaves an empty
     * line waiting. */
    {
        istringstream i("first line\nsecond\n");
        string l1, l2;
        getline(i, l1);
        getline(i, l2);
        CHECK(l1 == "first line" && l2 == "second");
    }
    {
        istringstream i("42\nrest of line\n");
        int n = 0; string rest;
        i >> n;
        getline(i, rest);
        CHECK(n == 42);
        CHECK(rest == "");                   /* the newline was still there */
        getline(i, rest);
        CHECK(rest == "rest of line");
    }
    {
        istringstream i("a,b,,c");
        string f; vector<string> fields;
        while (getline(i, f, ',')) fields.push_back(f);
        CHECK(fields.size() == 4);
        CHECK(fields[2] == "");              /* an empty field is a field */
        CHECK(fields[3] == "c");
    }

    /* get/peek/unget, the unformatted side */
    {
        istringstream i("abc");
        CHECK(i.peek() == 'a');
        char c;
        i.get(c);
        CHECK(c == 'a');
        i.unget();
        i.get(c);
        CHECK(c == 'a');
        i.ignore(1);
        CHECK(i.peek() == 'c');
    }

    /* A stringbuf grows as it is written, which moves the characters its
     * get area points at. Reading back after a large write is what
     * catches an implementation that set the areas only once. */
    {
        ostringstream o;
        for (int k = 0; k < 500; k++) o << k << ' ';
        string s = o.str();
        istringstream i(s);
        int seen = 0, val;
        while (i >> val) { CHECK(val == seen); seen++; }
        CHECK(seen == 500);
    }

    /* ---- the standard streams, which is the point of all of it ---- */
    cout << "iostreams: " << 1 << ' ' << 2.5 << ' ' << "ok" << endl;
    cout << setw(6) << setfill('.') << 42 << endl;
    cerr << "";                              /* exercised, prints nothing */
    cout.flush();
    DONE();
}
