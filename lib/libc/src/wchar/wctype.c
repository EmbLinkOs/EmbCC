/* Wide character classification, C11 §7.30.
 *
 * In the "C" locale a wide character below 128 classifies as its narrow
 * self, and everything above it classifies as nothing. That is not a
 * shortcut -- it is what the "C" locale means. It classifies exactly
 * the basic execution character set, and a library answering
 * `iswalpha(U'e' with an acute)` yes here would be claiming a locale it
 * does not have.
 *
 * A program doing real Unicode work needs property tables far larger
 * than any C locale provides; it wants ICU, not this.
 */
#include <wctype.h>
#include <ctype.h>
#include <string.h>

/* Below 128 the narrow classifier is the answer; at or above it, the
 * "C" locale says no. WEOF is deliberately included in "no" -- it is
 * not a character. */
#define NARROW(fn, c) ((unsigned)(c) < 128u ? fn((int)(c)) : 0)

int iswalnum(wint_t c)  { return NARROW(isalnum, c); }
int iswalpha(wint_t c)  { return NARROW(isalpha, c); }
int iswblank(wint_t c)  { return NARROW(isblank, c); }
int iswcntrl(wint_t c)  { return NARROW(iscntrl, c); }
int iswdigit(wint_t c)  { return NARROW(isdigit, c); }
int iswgraph(wint_t c)  { return NARROW(isgraph, c); }
int iswlower(wint_t c)  { return NARROW(islower, c); }
int iswprint(wint_t c)  { return NARROW(isprint, c); }
int iswpunct(wint_t c)  { return NARROW(ispunct, c); }
int iswspace(wint_t c)  { return NARROW(isspace, c); }
int iswupper(wint_t c)  { return NARROW(isupper, c); }
int iswxdigit(wint_t c) { return NARROW(isxdigit, c); }

/* A character with no case maps to itself, which is why these return
 * the input rather than something derived from it. */
wint_t towlower(wint_t c)
{
    return (unsigned)c < 128u ? (wint_t)tolower((int)c) : c;
}

wint_t towupper(wint_t c)
{
    return (unsigned)c < 128u ? (wint_t)toupper((int)c) : c;
}

/* ---- the extensible forms ---------------------------------------------
 *
 * A class looked up by NAME, so a program can ask about a category the
 * header has no function for. The set of names here is exactly the
 * twelve above; an unknown name returns zero, and iswctype of zero is
 * false, so a program that does not check still gets a defensible
 * answer rather than a crash.
 */
static const char *const class_names[] = {
    "alnum", "alpha", "blank", "cntrl", "digit", "graph",
    "lower", "print", "punct", "space", "upper", "xdigit"
};

wctype_t wctype(const char *name)
{
    if (!name)
        return 0;
    for (size_t i = 0; i < sizeof class_names / sizeof class_names[0]; i++)
        if (strcmp(name, class_names[i]) == 0)
            return (wctype_t)(i + 1);
    return 0;
}

int iswctype(wint_t c, wctype_t type)
{
    switch (type) {
    case 1:  return iswalnum(c);
    case 2:  return iswalpha(c);
    case 3:  return iswblank(c);
    case 4:  return iswcntrl(c);
    case 5:  return iswdigit(c);
    case 6:  return iswgraph(c);
    case 7:  return iswlower(c);
    case 8:  return iswprint(c);
    case 9:  return iswpunct(c);
    case 10: return iswspace(c);
    case 11: return iswupper(c);
    case 12: return iswxdigit(c);
    default: return 0;
    }
}

wctrans_t wctrans(const char *name)
{
    if (!name)
        return 0;
    if (strcmp(name, "tolower") == 0)
        return 1;
    if (strcmp(name, "toupper") == 0)
        return 2;
    return 0;
}

wint_t towctrans(wint_t c, wctrans_t trans)
{
    if (trans == 1)
        return towlower(c);
    if (trans == 2)
        return towupper(c);
    return c;
}
