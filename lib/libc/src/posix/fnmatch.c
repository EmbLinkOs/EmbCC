/* fnmatch: whether a string matches a shell wildcard pattern.
 *
 * One pass with ONE backtracking point, the last '*' seen -- which is
 * enough, and is what keeps a pattern like "*a*a*a*a*b" against a long
 * run of a's linear rather than exponential. The argument: when the
 * text after the last star fails, only that star stretching can help;
 * an earlier star stretching would have to re-match everything between
 * it and the last one, and the last star can already absorb that.
 *
 * FNM_PATHNAME changes one thing about it: no wildcard matches '/', so
 * a star may not stretch over one, and when the last star would have
 * to, nothing can help and the match fails outright.
 */
#include <fnmatch.h>
#include <ctype.h>
#include <string.h>

static int fold(int c, int flags)
{
    return flags & FNM_CASEFOLD ? tolower(c) : c;
}

static int in_class(const char *name, size_t n, int c)
{
    static const struct { const char *name; int (*is)(int); } cls[] = {
        { "alnum", isalnum }, { "alpha", isalpha }, { "blank", isblank },
        { "cntrl", iscntrl }, { "digit", isdigit }, { "graph", isgraph },
        { "lower", islower }, { "print", isprint }, { "punct", ispunct },
        { "space", isspace }, { "upper", isupper }, { "xdigit", isxdigit },
    };
    for (size_t i = 0; i < sizeof cls / sizeof cls[0]; i++)
        if (strlen(cls[i].name) == n && memcmp(cls[i].name, name, n) == 0)
            return cls[i].is(c) != 0;
    return -1;
}

/* One character of a bracket expression, at *pp: escaped, a collating
 * element [.c.] or an equivalence class [=c=] of a single character
 * (this library's only locale has no others), or itself. -1 when it is
 * malformed. */
static int bracket_char(const char **pp, int flags)
{
    const char *p = *pp;
    if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1]) {
        *pp = p + 2;
        return (unsigned char)p[1];
    }
    if (*p == '[' && (p[1] == '.' || p[1] == '=')) {
        if (!p[2] || p[3] != p[1] || p[4] != ']')
            return -1;
        *pp = p + 5;
        return (unsigned char)p[2];
    }
    *pp = p + 1;
    return (unsigned char)*p;
}

/* The bracket expression after '[' at p, against c: past its ']', with
 * *ok set, or NULL when it is not one -- unterminated or malformed --
 * and the '[' is then an ordinary character, as the shell has it. */
static const char *bracket(const char *p, int c, int flags, int *ok)
{
    int neg = 0, hit = 0;
    if (*p == '!' || *p == '^') {
        neg = 1;
        p++;
    }
    const char *first = p;
    for (;;) {
        if (!*p)
            return NULL;
        if (*p == ']' && p > first)
            break;
        if (p[0] == '[' && p[1] == ':') {
            const char *name = p + 2, *e = strstr(name, ":]");
            if (!e)
                return NULL;
            /* a class is not folded: [[:upper:]] is the upper case
             * letters under FNM_CASEFOLD too, as glibc has it */
            int r = in_class(name, (size_t)(e - name), c);
            if (r < 0)
                return NULL;
            hit |= r;
            p = e + 2;
            continue;
        }
        int lo = bracket_char(&p, flags), hi = lo;
        if (lo < 0)
            return NULL;
        if (p[0] == '-' && p[1] && p[1] != ']') {
            p++;
            hi = bracket_char(&p, flags);
            if (hi < 0)
                return NULL;
        }
        if (lo <= c && c <= hi)
            hit = 1;
        else if (flags & FNM_CASEFOLD) {
            int l = tolower(c), u = toupper(c);
            if ((lo <= l && l <= hi) || (lo <= u && u <= hi))
                hit = 1;
        }
    }
    *ok = hit != neg;
    return p + 1;
}

int fnmatch(const char *pat, const char *str, int flags)
{
    const char *p = pat, *s = str;
    const char *star_p = NULL, *star_s = NULL;
    int path = flags & FNM_PATHNAME;

    for (;;) {
        int c = (unsigned char)*s;
        /* A '.' that only a '.' may match: at the start, or of a path
         * component when '/' separates them. */
        int hidden = (flags & FNM_PERIOD) && c == '.' &&
                     (s == str || (path && s[-1] == '/'));
        switch (*p) {
        case 0:
            if (!c || ((flags & FNM_LEADING_DIR) && c == '/'))
                return 0;
            goto fail;
        case '?':
            if (!c || hidden || (path && c == '/'))
                goto fail;
            p++, s++;
            continue;
        case '*':
            if (hidden)
                goto fail;
            while (*p == '*')
                p++;
            star_p = p;
            star_s = s;
            continue;
        case '[': {
            int ok;
            const char *q = bracket(p + 1, c, flags, &ok);
            if (!q) {
                if (c != '[')
                    goto fail;
                p++, s++;
                continue;
            }
            if (!c || hidden || (path && c == '/') || !ok)
                goto fail;
            p = q, s++;
            continue;
        }
        case '\\':
            /* The escaped character, compared as any other. A '\' that
             * ends the pattern escapes nothing and matches nothing:
             * glibc's "trailing \ loses". */
            if (!(flags & FNM_NOESCAPE)) {
                if (!p[1])
                    return FNM_NOMATCH;
                p++;
            }
            /* fall through */
        default:
            if (!c || fold((unsigned char)*p, flags) != fold(c, flags))
                goto fail;
            p++, s++;
            continue;
        }
    fail:
        if (!star_p || !*star_s || (path && *star_s == '/'))
            return FNM_NOMATCH;
        p = star_p;
        s = ++star_s;
    }
}
