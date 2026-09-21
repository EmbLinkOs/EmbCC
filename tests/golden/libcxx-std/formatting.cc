/* <format>.
 *
 * Every check here compares against a string literal, which is the only
 * honest way to test a formatter: "it produced something" is what a
 * broken one does too. The expected strings are the ones the standard
 * specifies, and where this implementation is narrower than the
 * standard the test says so beside the check rather than quietly
 * expecting our own answer.
 */
#include "check.h"
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

/* A user type, to prove the extension point is real rather than a
 * list of special cases. Its formatter reads the spec it was given --
 * `{:>10}` on a Point must pad the whole "(1, 2)", not each number. */
struct Point { int x, y; };

template <>
struct std::formatter<Point> {
    static void format(const Point &p, const __fmt_spec &s, string &o)
    {
        string inner = std::format("({}, {})", p.x, p.y);
        __fmt::__format_str(inner.data(), inner.size(), s, o);
    }
};

static bool threw_format_error(void (*f)())
{
    try {
        f();
    } catch (const format_error &) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

int main()
{
    /* ---- no replacement fields --------------------------------------- */
    CHECK(format("") == "");
    CHECK(format("plain") == "plain");
    /* Doubled braces are the escape, and they are the reason a format
     * string can contain a brace at all. */
    CHECK(format("{{}}") == "{}");
    CHECK(format("{{{}}}", 1) == "{1}");
    CHECK(format("a{{b") == "a{b");

    /* ---- the defaults follow the TYPE, which is the whole point ------- */
    CHECK(format("{}", 42) == "42");
    CHECK(format("{}", -42) == "-42");
    CHECK(format("{}", 42u) == "42");
    CHECK(format("{}", 42L) == "42");
    CHECK(format("{}", (long long)-9007199254740993LL) == "-9007199254740993");
    CHECK(format("{}", true) == "true");
    CHECK(format("{}", false) == "false");
    CHECK(format("{}", 'x') == "x");
    CHECK(format("{}", "text") == "text");
    CHECK(format("{}", string("str")) == "str");
    CHECK(format("{}", string_view("view")) == "view");

    /* A char prints as a character and an int as a number -- with no
     * `%c` or `%d` to get the wrong way round. */
    CHECK(format("{} {}", 'A', 65) == "A 65");
    /* ... and each can be asked for the other. */
    CHECK(format("{:d} {:c}", 'A', 65) == "65 A");

    /* The most negative value: negating it in the signed domain is
     * undefined, so this is the check that it is not done that way. */
    CHECK(format("{}", (long long)(-9223372036854775807LL - 1))
          == "-9223372036854775808");
    CHECK(format("{}", (int)(-2147483647 - 1)) == "-2147483648");

    /* ---- argument ids ------------------------------------------------- */
    CHECK(format("{} {} {}", 1, 2, 3) == "1 2 3");
    CHECK(format("{2} {1} {0}", 1, 2, 3) == "3 2 1");
    CHECK(format("{0} {0} {0}", 7) == "7 7 7");
    /* Mixing automatic and manual is an error rather than a guess. */
    CHECK(threw_format_error([] { (void)format("{} {0}", 1); }));
    CHECK(threw_format_error([] { (void)format("{0} {}", 1); }));
    CHECK(threw_format_error([] { (void)format("{1}", 1); }));
    CHECK(threw_format_error([] { (void)format("{}"); }));

    /* ---- width and alignment ------------------------------------------ */
    /* The defaults differ by type: numbers right, everything else left.
     * That is what makes a column of numbers line up without saying so. */
    CHECK(format("[{:5}]", 42) == "[   42]");
    CHECK(format("[{:5}]", "ab") == "[ab   ]");
    CHECK(format("[{:5}]", 'c') == "[c    ]");
    CHECK(format("[{:<5}]", 42) == "[42   ]");
    CHECK(format("[{:>5}]", "ab") == "[   ab]");
    CHECK(format("[{:^5}]", 42) == "[ 42  ]");
    CHECK(format("[{:^6}]", "ab") == "[  ab  ]");
    /* A width narrower than the value never truncates. */
    CHECK(format("[{:2}]", 12345) == "[12345]");

    /* The fill is read by looking at the character AFTER it, so a fill
     * that is itself an alignment character works. A parser that tested
     * the first character for an align would read this as align-left. */
    CHECK(format("[{:*>5}]", 42) == "[***42]");
    CHECK(format("[{:*<5}]", 42) == "[42***]");
    CHECK(format("[{:*^6}]", 42) == "[**42**]");
    CHECK(format("[{:<<5}]", 42) == "[42<<<]");
    CHECK(format("[{:0>5}]", 42) == "[00042]");

    /* ---- sign ---------------------------------------------------------- */
    CHECK(format("{:+} {:+}", 5, -5) == "+5 -5");
    CHECK(format("{: } {: }", 5, -5) == " 5 -5");
    CHECK(format("{:-} {:-}", 5, -5) == "5 -5");

    /* ---- zero padding is an ALIGNMENT, not a fill --------------------- */
    /* The sign stays outside the zeros: "-0042", never "000-42". */
    CHECK(format("{:05}", 42) == "00042");
    CHECK(format("{:05}", -42) == "-0042");
    CHECK(format("{:+05}", 42) == "+0042");
    /* An explicit alignment turns zero padding off, which the standard
     * says and which is easy to get wrong: the `0` here changes
     * nothing, so the fill is still a space. */
    CHECK(format("{:<05}", 42) == "42   ");
    CHECK(format("{:^06}", 42) == "  42  ");

    /* ---- bases --------------------------------------------------------- */
    CHECK(format("{:x} {:X}", 255, 255) == "ff FF");
    CHECK(format("{:#x} {:#X}", 255, 255) == "0xff 0XFF");
    CHECK(format("{:o} {:#o}", 8, 8) == "10 0o10");
    CHECK(format("{:b} {:#b}", 5, 5) == "101 0b101");
    CHECK(format("{:#010x}", 255) == "0x000000ff");
    CHECK(format("{:b}", 0) == "0");
    CHECK(format("{:x}", 0) == "0");

    /* ---- floating point ------------------------------------------------ */
    /* The default is the shortest form that round-trips, so 1.5 is
     * "1.5" and not "1.500000" -- the one place format and printf
     * disagree by design. */
    CHECK(format("{}", 1.5) == "1.5");
    CHECK(format("{}", 0.0) == "0");
    CHECK(format("{}", -0.5) == "-0.5");
    CHECK(format("{:.2f}", 3.14159) == "3.14");
    CHECK(format("{:.0f}", 2.5) == "2");     /* banker's rounding, as printf */
    CHECK(format("{:.3f}", -1.5) == "-1.500");
    CHECK(format("{:8.2f}", 3.14159) == "    3.14");
    CHECK(format("{:<8.2f}", 3.14159) == "3.14    ");
    CHECK(format("{:08.2f}", -1.5) == "-0001.50");
    CHECK(format("{:+.1f}", 2.0) == "+2.0");
    CHECK(format("{:.2e}", 1234.5) == "1.23e+03");
    CHECK(format("{:.2E}", 1234.5) == "1.23E+03");

    /* A nan or an inf is not a number, so it is not zero-padded: the
     * alternative is "000-inf", which reads as a number and is not one. */
    double inf = __builtin_inf(), nan = __builtin_nan("");
    CHECK(format("{}", inf) == "inf");
    CHECK(format("{}", -inf) == "-inf");
    CHECK(format("{}", nan) == "nan" || format("{}", nan) == "-nan");
    CHECK(format("{:08}", inf).find("000") == string::npos);
    CHECK(format("{:>8}", inf) == "     inf");

    /* ---- strings and precision ----------------------------------------- */
    /* Precision TRUNCATES a string. It is the one thing `%.*s` and this
     * agree on exactly. */
    CHECK(format("{:.3}", "abcdef") == "abc");
    CHECK(format("{:.10}", "abc") == "abc");
    CHECK(format("{:>6.3}", "abcdef") == "   abc");
    CHECK(format("{:.0}", "abc") == "");
    CHECK(format("{}", (const char *)nullptr) == "");

    /* ---- nested width and precision ------------------------------------ */
    CHECK(format("{:{}}", 42, 5) == "   42");
    CHECK(format("{:.{}f}", 3.14159, 3) == "3.142");
    CHECK(format("{:{}.{}f}", 3.14159, 9, 2) == "     3.14");
    CHECK(format("{0:{1}}", "x", 4) == "x   ");
    /* A negative width is an error, not a cast to an enormous size. */
    CHECK(threw_format_error([] { (void)format("{:{}}", 1, -1); }));
    CHECK(threw_format_error([] { (void)format("{:{}}", 1, "no"); }));

    /* ---- bool and pointer ---------------------------------------------- */
    CHECK(format("{:d} {:d}", true, false) == "1 0");
    CHECK(format("{:>7}", true) == "   true");
    CHECK(format("{:#x}", true) == "0x1");
    int n = 0;
    string ps = format("{}", (void *)&n);
    CHECK(ps.size() > 2 && ps[0] == '0' && ps[1] == 'x');
    CHECK(format("{}", (void *)nullptr) == "0x0");

    /* ---- a user type --------------------------------------------------- */
    Point p{1, 2};
    CHECK(format("{}", p) == "(1, 2)");
    /* The spec reaches the user's formatter: the whole value is padded,
     * not each number inside it. */
    CHECK(format("[{:>10}]", p) == "[    (1, 2)]");
    CHECK(format("{} and {}", p, Point{3, 4}) == "(1, 2) and (3, 4)");

    /* ---- malformed strings throw rather than guess --------------------- */
    CHECK(threw_format_error([] { (void)format("{", 1); }));
    CHECK(threw_format_error([] { (void)format("}", 1); }));
    CHECK(threw_format_error([] { (void)format("{:", 1); }));
    CHECK(threw_format_error([] { (void)format("{0", 1); }));
    CHECK(threw_format_error([] { (void)format("{:.}", 1.0); }));
    CHECK(threw_format_error([] { (void)format("{:{}", 1, 2); }));

    /* ---- format_to, format_to_n, formatted_size ------------------------ */
    {
        string dst;
        format_to(back_inserter(dst), "{}-{}", 1, 2);
        CHECK(dst == "1-2");
    }
    {
        char buf[8];
        for (size_t i = 0; i < sizeof buf; i++)
            buf[i] = '#';
        auto r = format_to_n(buf, 4, "{}", 1234567);
        /* `size` is what the WHOLE output would have been, which is how
         * a caller learns the buffer was too small -- snprintf's
         * convention, and for the same reason. */
        CHECK(r.size == 7);
        CHECK(buf[0] == '1' && buf[3] == '4' && buf[4] == '#');
        CHECK(r.out == buf + 4);
    }
    CHECK(formatted_size("{}", 12345) == 5);
    CHECK(formatted_size("{:>10}", "x") == 10);

    /* ---- vformat: the erased form the templates all funnel into -------- */
    {
        auto st = make_format_args(7, "seven");
        CHECK(vformat("{} is {}", format_args(st.__v, 2)) == "7 is seven");
    }

    /* ---- and it composes ----------------------------------------------- */
    {
        vector<Point> pts{{1, 2}, {30, 40}};
        string table;
        for (const Point &q : pts)
            table += format("{:>10}|{:>5}|{:>5}\n", q, q.x, q.y);
        CHECK(table ==
              "    (1, 2)|    1|    2\n"
              "  (30, 40)|   30|   40\n");
    }

    /* A format string built at run time is fine -- it is checked when it
     * is used, which is this implementation's limit and its one
     * flexibility. */
    {
        string f = "{:>";
        f += "6";
        f += "}";
        CHECK(format(f, 42) == "    42");
    }

    DONE();
}
