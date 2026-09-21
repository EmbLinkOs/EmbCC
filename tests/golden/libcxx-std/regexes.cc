/* <regex>.
 *
 * A regex engine is mostly right or completely wrong, and the places it
 * is completely wrong are always the same ones. So the checks here are
 * weighted towards them rather than spread evenly:
 *
 *   GREEDY vs LAZY. `<.+>` against "<a><b>" matches the whole thing and
 *     `<.+?>` matches "<a>". Getting this backwards produces plausible
 *     output on simple inputs and destroys any parser built on it.
 *
 *   LEFTMOST, not longest. `(a|ab)` against "ab" matches "a": the
 *     alternation takes the first branch that lets the REST succeed, and
 *     here nothing follows, so "a" wins. POSIX regex answers "ab" to the
 *     same question; ECMAScript does not, and the standard says
 *     ECMAScript.
 *
 *   EMPTY MATCHES. `a*` matches the empty string everywhere, so anything
 *     that loops over matches must advance past one or never terminate.
 *     regex_replace, regex_iterator and `(a*)*` are three separate
 *     places this has to be handled.
 *
 *   BACKTRACKING OUT OF A CAPTURE. `(a)|b` against "b" must report group
 *     1 as unmatched: the first alternative matched and was abandoned,
 *     and a capture recorded on the way in and not undone would survive
 *     it.
 */
#include "check.h"
#include <regex>
#include <string>
#include <vector>

using namespace std;

static bool m(const char *pat, const char *s)
{ return regex_match(s, regex(pat)); }
static bool sr(const char *pat, const char *s)
{ return regex_search(s, regex(pat)); }

int main()
{
    /* ---- match is the WHOLE subject; search is any part of it -------- */
    CHECK(m("hello", "hello"));
    CHECK(!m("hello", "hello world"));    /* the classic first surprise */
    CHECK(sr("hello", "hello world"));
    CHECK(!sr("hello", "goodbye"));
    CHECK(m("", ""));
    CHECK(!m("", "x"));

    /* ---- literals, '.', and what '.' does not match ------------------ */
    CHECK(m("a.c", "abc"));
    CHECK(m("a.c", "a.c"));
    /* `.` is every character except a newline -- the ECMAScript rule,
     * and the one that keeps a line-oriented pattern on its line. */
    CHECK(!m("a.c", "a\nc"));
    CHECK(!m("a.c", "ac"));

    /* ---- character classes ------------------------------------------- */
    CHECK(m("[abc]+", "cabba"));
    CHECK(!m("[abc]+", "cabda"));
    CHECK(m("[a-z]+", "hello"));
    CHECK(!m("[a-z]+", "Hello"));
    CHECK(m("[^0-9]+", "abc"));
    CHECK(!m("[^0-9]+", "ab3"));
    CHECK(m("[a-zA-Z0-9_]+", "a_9Z"));
    /* A '-' at either end is a literal, not a range. */
    CHECK(m("[a-]+", "a-a"));
    CHECK(m("[-a]+", "-aa"));
    /* An escaped ']' inside a class. (A bare ']' right after '[' is the
     * POSIX reading and is accepted here as an extension, but it is not
     * checked: ECMAScript reads `[]` as an EMPTY class, so the two
     * grammars disagree about `[]]` and portable patterns escape it.) */
    CHECK(m("[\\]]", "]"));

    /* ---- the class escapes, inside and outside brackets --------------- */
    CHECK(m("\\d+", "12345"));
    CHECK(!m("\\d+", "12a45"));
    CHECK(m("\\D+", "abc"));
    CHECK(m("\\w+", "a_1"));
    CHECK(!m("\\w+", "a b"));
    CHECK(m("\\W+", " -+"));
    CHECK(m("\\s+", " \t\n"));
    CHECK(m("\\S+", "abc"));
    CHECK(m("[\\d\\s]+", "1 2\t3"));
    CHECK(!m("[\\d\\s]+", "1a2"));
    CHECK(m("\\n", "\n"));
    CHECK(m("\\t", "\t"));
    CHECK(m("\\.", "."));
    CHECK(!m("\\.", "x"));
    CHECK(m("\\x41", "A"));
    CHECK(m("[\\x41-\\x43]+", "ABC"));

    /* ---- quantifiers -------------------------------------------------- */
    CHECK(m("a*", ""));
    CHECK(m("a*", "aaaa"));
    CHECK(!m("a+", ""));
    CHECK(m("a+", "a"));
    CHECK(m("ab?c", "abc"));
    CHECK(m("ab?c", "ac"));
    CHECK(m("a{3}", "aaa"));
    CHECK(!m("a{3}", "aa"));
    CHECK(!m("a{3}", "aaaa"));
    CHECK(m("a{2,}", "aaaa"));
    CHECK(!m("a{2,}", "a"));
    CHECK(m("a{2,4}", "aaa"));
    CHECK(!m("a{2,4}", "aaaaa"));
    /* A '{' that does not look like a quantifier is taken as a literal
     * here, as JavaScript does; the standard's grammar makes it an
     * error and libc++ rejects it, so a portable pattern escapes it and
     * this is not checked either way. */
    CHECK(m("a\\{b", "a{b"));

    /* ---- greedy against lazy, the check that matters ------------------ */
    {
        cmatch r;
        CHECK(regex_search("<a><b>", r, regex("<.+>")));
        CHECK(r.str(0) == "<a><b>");       /* greedy takes everything */
        CHECK(regex_search("<a><b>", r, regex("<.+?>")));
        CHECK(r.str(0) == "<a>");          /* lazy stops at the first */

        CHECK(regex_search("aaa", r, regex("a+")));
        CHECK(r.str(0) == "aaa");
        CHECK(regex_search("aaa", r, regex("a+?")));
        CHECK(r.str(0) == "a");
        CHECK(regex_search("aaa", r, regex("a{1,3}?")));
        CHECK(r.str(0) == "a");

        /* Greedy still backs off when it has to: `.*b` cannot keep the
         * b, so the star gives it up. */
        CHECK(regex_match("aaab", r, regex(".*b")));
        CHECK(r.str(0) == "aaab");
        CHECK(regex_match("aXbXc", r, regex("(.*)X(.*)")));
        CHECK(r.str(1) == "aXb" && r.str(2) == "c");   /* first is greedy */
    }

    /* ---- alternation is LEFTMOST, not longest ------------------------- */
    {
        cmatch r;
        CHECK(regex_search("ab", r, regex("a|ab")));
        CHECK(r.str(0) == "a");
        /* ... but the whole continuation is retried, so a branch that
         * fails downstream gives way to the next. */
        CHECK(regex_match("abc", r, regex("(a|ab)c*")));
        CHECK(regex_match("abc", r, regex("(a|ab)(bc|c)")));
        CHECK(r.str(1) == "a" && r.str(2) == "bc");
        CHECK(m("cat|dog|bird", "dog"));
        CHECK(!m("cat|dog|bird", "fish"));
        CHECK(m("(|a)", ""));              /* an empty alternative */
    }

    /* ---- groups and captures ------------------------------------------ */
    {
        cmatch r;
        CHECK(regex_match("2024-09-20", r,
                          regex("(\\d{4})-(\\d{2})-(\\d{2})")));
        CHECK(r.size() == 4);              /* group 0 is the whole match */
        CHECK(r.str(0) == "2024-09-20");
        CHECK(r.str(1) == "2024" && r.str(2) == "09" && r.str(3) == "20");
        CHECK(r.position(1) == 0 && r.position(2) == 5);
        CHECK(r.length(1) == 4 && r.length(3) == 2);
        CHECK(r[1].matched && r[2].matched);

        /* Non-capturing groups do not take a number. */
        CHECK(regex_match("abab", r, regex("(?:ab)(ab)")));
        CHECK(r.size() == 2 && r.str(1) == "ab");

        /* An optional group that did not participate is UNMATCHED, and
         * that is different from matching the empty string. */
        CHECK(regex_match("ac", r, regex("a(b)?c")));
        CHECK(!r[1].matched);
        CHECK(r.str(1) == "");
        CHECK(regex_match("a", r, regex("a(b*)")));
        CHECK(r[1].matched && r.str(1) == "");   /* matched, and empty */

        /* Backtracking out of a capture must leave no trace. */
        CHECK(regex_match("b", r, regex("(a)|b")));
        CHECK(!r[1].matched);

        /* A repeated group keeps its LAST iteration. */
        CHECK(regex_match("abc", r, regex("(.)+")));
        CHECK(r.str(1) == "c");

        /* Nesting: the numbers follow the open parentheses left to
         * right, which is why the outer group is 1. */
        CHECK(regex_match("abcd", r, regex("((a)(b))(c)(d)")));
        CHECK(r.size() == 6);
        CHECK(r.str(1) == "ab" && r.str(2) == "a" && r.str(3) == "b");
        CHECK(r.str(4) == "c" && r.str(5) == "d");
    }

    /* ---- anchors ------------------------------------------------------- */
    {
        CHECK(sr("^abc", "abcdef"));
        CHECK(!sr("^abc", "xabcdef"));
        CHECK(sr("def$", "abcdef"));
        CHECK(!sr("def$", "abcdefx"));
        CHECK(m("^abc$", "abc"));

        cmatch r;
        /* \b is a zero-width test between a word and a non-word
         * character, which is why it finds "cat" in "the cat sat" and
         * not in "concatenate". */
        CHECK(regex_search("the cat sat", r, regex("\\bcat\\b")));
        CHECK(!regex_search("concatenate", r, regex("\\bcat\\b")));
        CHECK(regex_search("concatenate", r, regex("\\Bcat\\B")));
        CHECK(sr("\\bhello", "hello"));    /* the start counts as a boundary */
        CHECK(sr("world\\b", "world"));
    }

    /* Multiline makes ^ and $ mean line, not subject. */
    {
        cmatch r;
        regex ml("^b$", regex_constants::multiline);
        CHECK(regex_search("a\nb\nc", r, ml));
        CHECK(!regex_search("a\nb\nc", r, regex("^b$")));
    }

    /* ---- backreferences: the feature that forces a backtracker -------- */
    {
        cmatch r;
        CHECK(regex_match("abcabc", r, regex("(abc)\\1")));
        CHECK(regex_match("aa", r, regex("(a)\\1")));
        CHECK(!regex_match("ab", r, regex("(a)\\1")));
        /* A doubled word, which is what this is always used for. */
        CHECK(regex_search("the the cat", r, regex("\\b(\\w+) \\1\\b")));
        CHECK(r.str(1) == "the");
        CHECK(!regex_search("the cat sat", r, regex("\\b(\\w+) \\1\\b")));
        /* A backreference to a group that has not participated matches
         * the empty string here, as JavaScript does. Implementations
         * differ -- libc++ makes it fail -- so it is described rather
         * than asserted. */
    }

    /* ---- lookahead: zero width, and the negative form ----------------- */
    {
        cmatch r;
        CHECK(regex_search("foobar", r, regex("foo(?=bar)")));
        CHECK(r.str(0) == "foo");          /* "bar" is NOT consumed */
        CHECK(!regex_search("foobaz", r, regex("foo(?=bar)")));
        CHECK(regex_search("foobaz", r, regex("foo(?!bar)")));
        CHECK(!regex_search("foobar", r, regex("foo(?!bar)")));
        /* A password-shaped rule: at least one digit somewhere. */
        CHECK(m("(?=.*\\d).{6,}", "abc1def"));
        CHECK(!m("(?=.*\\d).{6,}", "abcdefg"));
    }

    /* ---- icase --------------------------------------------------------- */
    {
        regex ci("hello", regex_constants::icase);
        CHECK(regex_match("HELLO", ci));
        CHECK(regex_match("HeLLo", ci));
        CHECK(!regex_match("hello!", ci));
        regex cc("[a-f]+", regex_constants::icase);
        CHECK(regex_match("AbCdEf", cc));
        CHECK(!regex_match("AbCdEg", cc));
        /* icase reaches a backreference too. */
        regex br("(ab)\\1", regex_constants::icase);
        CHECK(regex_match("abAB", br));
    }

    /* ---- empty matches, in all three places they bite ------------------ */
    {
        cmatch r;
        /* A nested star over something that can match empty must not
         * loop forever. */
        CHECK(regex_match("aaa", r, regex("(a*)*")));
        CHECK(regex_match("", r, regex("(a*)*")));
        CHECK(regex_match("", r, regex("(a?)*")));
        CHECK(regex_match("b", r, regex("(a*)*b")));
    }

    /* ---- prefix, suffix and position ------------------------------------ */
    {
        cmatch r;
        CHECK(regex_search("xxabcyy", r, regex("abc")));
        CHECK(r.prefix().str() == "xx");
        CHECK(r.suffix().str() == "yy");
        CHECK(r.position(0) == 2);
        CHECK(r.length(0) == 3);
        CHECK(regex_search("abcyy", r, regex("abc")));
        CHECK(!r.prefix().matched && r.suffix().matched);
    }

    /* ---- std::string subjects, and smatch -------------------------------- */
    {
        string s = "key = value";
        smatch r;
        CHECK(regex_match(s, r, regex("(\\w+)\\s*=\\s*(\\w+)")));
        CHECK(r.str(1) == "key" && r.str(2) == "value");
        CHECK(regex_search(s, r, regex("=")));
        CHECK(r.prefix().str() == "key ");
    }

    /* ---- regex_replace ---------------------------------------------------- */
    {
        CHECK(regex_replace(string("a1b2c3"), regex("\\d"), string("#"))
              == "a#b#c#");
        CHECK(regex_replace(string("hello world"), regex("o"), string("0"))
              == "hell0 w0rld");
        /* $1 and $& in the format string. */
        CHECK(regex_replace(string("John Smith"),
                            regex("(\\w+) (\\w+)"), string("$2, $1"))
              == "Smith, John");
        CHECK(regex_replace(string("abc"), regex("b"), string("[$&]"))
              == "a[b]c");
        CHECK(regex_replace(string("abc"), regex("b"), string("$$"))
              == "a$c");
        /* format_first_only stops after one. */
        CHECK(regex_replace(string("aaa"), regex("a"), string("b"),
                            regex_constants::format_first_only)
              == "baa");
        /* An empty match must still advance: this is the loop that hangs
         * if it does not. */
        CHECK(regex_replace(string("abc"), regex("x*"), string("-"))
              == "-a-b-c-");
        CHECK(regex_replace(string(""), regex("x*"), string("-")) == "-");
        /* No match at all leaves the subject alone. */
        CHECK(regex_replace(string("abc"), regex("z"), string("!")) == "abc");
    }

    /* ---- regex_iterator: every match in turn ------------------------------ */
    {
        string s = "one 2 three 44 five 666";
        regex num("\\d+");
        vector<string> found;
        for (sregex_iterator i(s.begin(), s.end(), num), e; i != e; ++i)
            found.push_back(i->str());
        CHECK(found.size() == 3);
        CHECK(found[0] == "2" && found[1] == "44" && found[2] == "666");

        /* Captures, per match. */
        string kv = "a=1,b=22,c=333";
        regex pair_re("(\\w+)=(\\d+)");
        string joined;
        for (sregex_iterator i(kv.begin(), kv.end(), pair_re), e; i != e; ++i)
            joined += i->str(1) + ":" + i->str(2) + ";";
        CHECK(joined == "a:1;b:22;c:333;");

        /* No matches at all: begin == end immediately. */
        string none = "xyz";
        sregex_iterator i(none.begin(), none.end(), num), e;
        CHECK(i == e);
    }

    /* ---- regex_token_iterator: -1 is a splitter --------------------------- */
    {
        string s = "a,b,,c";
        regex comma(",");
        vector<string> parts;
        for (sregex_token_iterator i(s.begin(), s.end(), comma, -1), e;
             i != e; ++i)
            parts.push_back(i->str());
        CHECK(parts.size() == 4);
        CHECK(parts[0] == "a" && parts[1] == "b");
        CHECK(parts[2] == "" && parts[3] == "c");   /* the empty field */

        /* A chosen group instead. */
        string kv = "a=1 b=2";
        regex pr("(\\w+)=(\\d+)");
        vector<string> keys;
        for (sregex_token_iterator i(kv.begin(), kv.end(), pr, 1), e;
             i != e; ++i)
            keys.push_back(i->str());
        CHECK(keys.size() == 2 && keys[0] == "a" && keys[1] == "b");
    }

    /* ---- a bad pattern is an exception, not a wrong answer ---------------- */
    {
        int caught = 0;
        try { regex r("("); }     catch (const regex_error &) { caught++; }
        try { regex r("["); }     catch (const regex_error &) { caught++; }
        try { regex r("a{2,1}"); } catch (const regex_error &) { caught++; }
        try { regex r("[z-a]"); } catch (const regex_error &) { caught++; }
        try { regex r("*a"); }    catch (const regex_error &) { caught++; }
        try { regex r("a)"); }    catch (const regex_error &) { caught++; }
        CHECK(caught == 6);
        /* regex_error is a runtime_error, so a caller that only knows
         * the standard hierarchy still catches it. */
        bool as_runtime = false;
        try { regex r("("); } catch (const runtime_error &) { as_runtime = true; }
        CHECK(as_runtime);
    }

    /* ---- mark_count and a copied regex ------------------------------------ */
    {
        regex r("(a)(b)(?:c)(d)");
        CHECK(r.mark_count() == 3);        /* (?:) does not count */
        regex copy = r;                    /* shares the tree, does not
                                            * rebuild or double-free it */
        cmatch mm;
        CHECK(regex_match("abcd", mm, copy));
        CHECK(mm.str(3) == "d");
        regex empty;
        CHECK(empty.mark_count() == 0);
    }

    /* ---- something real: a tokenizer --------------------------------------- */
    {
        string src = "let x = 42 + foo(3);";
        regex tok("[A-Za-z_]\\w*|\\d+|[=+;()]");
        vector<string> ts;
        for (sregex_iterator i(src.begin(), src.end(), tok), e; i != e; ++i)
            ts.push_back(i->str());
        CHECK(ts.size() == 10);
        CHECK(ts[0] == "let" && ts[1] == "x" && ts[2] == "=");
        CHECK(ts[3] == "42" && ts[4] == "+" && ts[5] == "foo");
        CHECK(ts[6] == "(" && ts[7] == "3" && ts[8] == ")" && ts[9] == ";");
    }

    DONE();
}
