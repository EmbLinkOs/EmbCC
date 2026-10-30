/* <regex.h>: 88 patterns, flags and strings, each with glibc 2.36's
 * answer -- its compile error, or no match, or the match and every
 * group's offsets. Basic and extended syntax, the GNU extensions,
 * REG_ICASE, REG_NEWLINE, REG_NOTBOL/NOTEOL, back-references, bracket
 * classes, intervals, POSIX's leftmost-longest. The table was made by
 * running the same calls against glibc and printing what it said. */
// expect-exit: 42
// needs-libc: <regex.h>, from lib/libc
#define _GNU_SOURCE 1
#include <langinfo.h>
#include <nl_types.h>
#include <regex.h>
#include <string.h>

static const struct {
    const char *pat;
    int cflags;
    const char *s;
    int eflags;
    int comp;                  /* regcomp's error, 0 for none */
    int n;                     /* groups reported, -1 for no match */
    int off[14];
} cases[] = {
    { "abc", 0, "xxabcxx", 0, 0, 1, { 2, 5 } },
    { "a*", 0, "baaa", 0, 0, 1, { 0, 0 } },
    { "a*", 0, "aaab", 0, 0, 1, { 0, 3 } },
    { "a\\(b*\\)c", 0, "xabbbcx", 0, 0, 2, { 1, 6, 2, 5 } },
    { "a(b*)c", REG_EXTENDED, "xabbbcx", 0, 0, 2, { 1, 6, 2, 5 } },
    { "(a|ab)(c|bcd)(d*)", REG_EXTENDED, "abcd", 0, 0, 4, { 0, 4, 0, 1, 1, 4, 4, 4 } },
    { "(a*)*", REG_EXTENDED, "aaa", 0, 0, 2, { 0, 3, 0, 3 } },
    { "(a*)+", REG_EXTENDED, "aaa", 0, 0, 2, { 0, 3, 0, 3 } },
    { "(a|b)*c", REG_EXTENDED, "ababc", 0, 0, 2, { 0, 5, 3, 4 } },
    { "x(a|b)*", REG_EXTENDED, "xabab", 0, 0, 2, { 0, 5, 4, 5 } },
    { "(wee|week)(knights|night)", REG_EXTENDED, "weeknights", 0, 0, 3, { 0, 10, 0, 3, 3, 10 } },
    { "(.*)(.*)", REG_EXTENDED, "abc", 0, 0, 3, { 0, 3, 0, 3, 3, 3 } },
    { "(.*)x(.*)", REG_EXTENDED, "axbxc", 0, 0, 3, { 0, 5, 0, 3, 4, 5 } },
    { "([a-c]+)([b-d]+)", REG_EXTENDED, "abcd", 0, 0, 3, { 0, 4, 0, 3, 3, 4 } },
    { "^abc$", 0, "abc", 0, 0, 1, { 0, 3 } },
    { "^abc$", 0, "xabc", 0, 0, -1, { 0 } },
    { "^b", REG_EXTENDED | REG_NEWLINE, "a\nb", 0, 0, 1, { 2, 3 } },
    { "a$", REG_EXTENDED | REG_NEWLINE, "a\nb", 0, 0, 1, { 0, 1 } },
    { "^b", REG_EXTENDED, "a\nb", 0, 0, -1, { 0 } },
    { "a.b", REG_EXTENDED | REG_NEWLINE, "a\nb", 0, 0, -1, { 0 } },
    { "a.b", REG_EXTENDED, "a\nb", 0, 0, 1, { 0, 3 } },
    { "a[^x]b", REG_EXTENDED | REG_NEWLINE, "a\nb", 0, 0, -1, { 0 } },
    { "^a", REG_EXTENDED, "abc", REG_NOTBOL, 0, -1, { 0 } },
    { "c$", REG_EXTENDED, "abc", REG_NOTEOL, 0, -1, { 0 } },
    { "\\(a\\)\\1", 0, "xaax", 0, 0, 2, { 1, 3, 1, 2 } },
    { "(a+)b\\1", REG_EXTENDED, "aaabaa", 0, 0, 2, { 1, 6, 1, 3 } },
    { "\\(.\\)\\1", 0, "abccd", 0, 0, 2, { 2, 4, 2, 3 } },
    { "a\\{2,3\\}", 0, "aaaa", 0, 0, 1, { 0, 3 } },
    { "a{2,3}", REG_EXTENDED, "aaaa", 0, 0, 1, { 0, 3 } },
    { "a{2}", REG_EXTENDED, "aaaa", 0, 0, 1, { 0, 2 } },
    { "a{,2}", REG_EXTENDED, "aaa", 0, 0, 1, { 0, 2 } },
    { "a{2,1}", REG_EXTENDED, "a", 0, 10, 0, { 0 } },
    { "a\\{1", 0, "a", 0, 9, 0, { 0 } },
    { "a{1", REG_EXTENDED, "a", 0, 9, 0, { 0 } },
    { "a{x", REG_EXTENDED, "a{x", 0, 9, 0, { 0 } },
    { "[[:alpha:]]+", REG_EXTENDED, "12abc34", 0, 0, 1, { 2, 5 } },
    { "[[:digit:][:space:]]+", REG_EXTENDED, "ab1 2 3c", 0, 0, 1, { 2, 7 } },
    { "[[:foo:]]", REG_EXTENDED, "a", 0, 4, 0, { 0 } },
    { "[a-]+", REG_EXTENDED, "x-a-", 0, 0, 1, { 1, 4 } },
    { "[]a]+", REG_EXTENDED, "x]a]", 0, 0, 1, { 1, 4 } },
    { "[^]a]", REG_EXTENDED, "]ab", 0, 0, 1, { 2, 3 } },
    { "[z-a]", REG_EXTENDED, "a", 0, 11, 0, { 0 } },
    { "[.-.]", REG_EXTENDED, "-", 0, 0, -1, { 0 } },
    { "[[=a=]]", REG_EXTENDED, "bab", 0, 0, 1, { 1, 2 } },
    { "[[.a.]-c]+", REG_EXTENDED, "xabcd", 0, 0, 1, { 1, 4 } },
    { "ABC", REG_ICASE, "xabcx", 0, 0, 1, { 1, 4 } },
    { "[a-c]+", REG_EXTENDED | REG_ICASE, "xABCx", 0, 0, 1, { 1, 4 } },
    { "(a)\\1", REG_EXTENDED | REG_ICASE, "aA", 0, 0, 2, { 0, 2, 0, 1 } },
    { "a+?", REG_EXTENDED, "aaa", 0, 0, 1, { 0, 3 } },
    { "*a", 0, "x*a", 0, 0, 1, { 1, 3 } },
    { "*a", REG_EXTENDED, "x*a", 0, 13, 0, { 0 } },
    { "^*a", 0, "*a", 0, 0, 1, { 0, 2 } },
    { "a**", 0, "aaa", 0, 13, 0, { 0 } },
    { "a\\+", 0, "aaa", 0, 0, 1, { 0, 3 } },
    { "a\\?b", 0, "b", 0, 0, 1, { 0, 1 } },
    { "a\\|b", 0, "xb", 0, 0, 1, { 1, 2 } },
    { "a|b|", REG_EXTENDED, "x", 0, 0, 1, { 0, 0 } },
    { "()", REG_EXTENDED, "x", 0, 0, 2, { 0, 0, 0, 0 } },
    { "\\<foo\\>", 0, "a foo b", 0, 0, 1, { 2, 5 } },
    { "\\bfoo", REG_EXTENDED, "xfoo foo", 0, 0, 1, { 5, 8 } },
    { "o\\B", REG_EXTENDED, "foo", 0, 0, 1, { 1, 2 } },
    { "\\w+", REG_EXTENDED, "  ab_1 ", 0, 0, 1, { 2, 6 } },
    { "\\W+", REG_EXTENDED, "ab, cd", 0, 0, 1, { 2, 4 } },
    { "\\s+\\S", REG_EXTENDED, "a  b", 0, 0, 1, { 1, 4 } },
    { "a\\`", REG_EXTENDED, "a", 0, 0, -1, { 0 } },
    { "\\'", REG_EXTENDED, "ab", 0, 0, 1, { 2, 2 } },
    { "(", REG_EXTENDED, "a", 0, 8, 0, { 0 } },
    { ")", REG_EXTENDED, "a)", 0, 0, 1, { 1, 2 } },
    { "a\\)", 0, "a", 0, 8, 0, { 0 } },
    { "\\(a", 0, "a", 0, 8, 0, { 0 } },
    { "[abc", REG_EXTENDED, "a", 0, 7, 0, { 0 } },
    { "a\\", REG_EXTENDED, "a", 0, 5, 0, { 0 } },
    { "\\1", REG_EXTENDED, "a", 0, 6, 0, { 0 } },
    { "(a)\\2", REG_EXTENDED, "a", 0, 6, 0, { 0 } },
    { "a||b", REG_EXTENDED, "b", 0, 0, 1, { 0, 1 } },
    { "(a|b)c|d", REG_EXTENDED, "xbc", 0, 0, 2, { 1, 3, 1, 2 } },
    { "x*", REG_EXTENDED, "", 0, 0, 1, { 0, 0 } },
    { "", REG_EXTENDED, "abc", 0, 0, 1, { 0, 0 } },
    { "$", REG_EXTENDED, "ab", 0, 0, 1, { 2, 2 } },
    { "(a)(b)?", REG_EXTENDED, "a", 0, 0, 3, { 0, 1, 0, 1, -1, -1 } },
    { "(a(b)?)+", REG_EXTENDED, "aba", 0, 0, 3, { 0, 3, 2, 3, 1, 2 } },
    { "(a*)b\\1", REG_EXTENDED, "aabaa", 0, 0, 2, { 0, 5, 0, 2 } },
    { "[[:upper:]]+", REG_ICASE | REG_EXTENDED, "abC", 0, 0, 1, { 0, 3 } },
    { "abc", REG_EXTENDED | REG_NOSUB, "xabc", 0, 0, 0, { 0 } },
    { "(.)(.)(.)(.)(.)(.)", REG_EXTENDED, "abcdefg", 0, 0, 6, { 0, 6, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5 } },
    { "\\(ab*\\)*c", 0, "abbabc", 0, 0, 2, { 0, 6, 3, 5 } },
    { "x.*y", REG_EXTENDED, "x\ny", 0, 0, 1, { 0, 3 } },
    { "[[:alnum:]_]+@[a-z]+\\.[a-z]{2,}", REG_EXTENDED, "mail me@host.org now", 0, 0, 1, { 5, 16 } },
};

int main(void)
{
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        regex_t re;
        int e = regcomp(&re, cases[i].pat, cases[i].cflags);
        if (e != cases[i].comp)
            return 1;
        if (e)
            continue;
        regmatch_t m[7];
        memset(m, 0x7f, sizeof m);
        int r = regexec(&re, cases[i].s, 7, m, cases[i].eflags);
        if (cases[i].n < 0) {
            if (r != REG_NOMATCH)
                return 2;
        } else if (r != 0) {
            return 3;
        } else if (!(cases[i].cflags & REG_NOSUB)) {
            for (int g = 0; g < cases[i].n; g++)
                if (m[g].rm_so != cases[i].off[2 * g] ||
                    m[g].rm_eo != cases[i].off[2 * g + 1])
                    return 4;
        }
        regfree(&re);
    }
    /* REG_STARTEND: the search starts at rm_so, but ^ is the string's */
    regex_t re;
    regmatch_t m[1] = { { 1, 3 } };
    if (regcomp(&re, "^b", REG_EXTENDED) ||
        regexec(&re, "abc", 1, m, REG_STARTEND) != REG_NOMATCH)
        return 5;
    regfree(&re);
    m[0].rm_so = 1;
    m[0].rm_eo = 3;
    if (regcomp(&re, "c", REG_EXTENDED) ||
        regexec(&re, "abc", 1, m, REG_STARTEND) || m[0].rm_so != 2 ||
        m[0].rm_eo != 3)
        return 6;
    regfree(&re);
    /* regerror: the whole length, and a cut copy */
    char b[8];
    if (regerror(REG_EBRACK, NULL, b, sizeof b) != 31 || strcmp(b, "Unmatch"))
        return 7;
    /* and the "C" locale's own words, which glibc's are too */
    if (strcmp(nl_langinfo(D_FMT), "%m/%d/%y") || strcmp(nl_langinfo(DAY_1), "Sunday") ||
        strcmp(nl_langinfo(ABMON_12), "Dec") || strcmp(nl_langinfo(RADIXCHAR), ".") ||
        strcmp(nl_langinfo(YESEXPR), "^[yY]"))
        return 8;
    nl_catd cat = catopen("embcc-no-such-catalog", 0);
    if (cat != (nl_catd)-1 || strcmp(catgets(cat, 1, 1, "kept"), "kept"))
        return 9;
    return 42;
}
